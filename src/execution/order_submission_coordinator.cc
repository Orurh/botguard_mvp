#include "order_submission_coordinator.h"

#include <chrono>
#include <utility>

namespace botguard::execution {
namespace {

[[nodiscard]] bool ApplySubmitOutcome(risk::OrderRegistry& registry, risk::ClientOrderId client_order_id, VenueSubmitResult submit_result) {
  risk::OrderState state = risk::OrderState::kUnknown;

  switch (submit_result.outcome) {
    case SubmitOutcome::kOpen:
      state = risk::OrderState::kOpen;
      break;

    case SubmitOutcome::kFilled:
      state = risk::OrderState::kFilled;
      break;

    case SubmitOutcome::kRejected:
      state = risk::OrderState::kRejected;
      break;

    case SubmitOutcome::kUnknown:
      state = risk::OrderState::kUnknown;
      break;
  }

  return registry.ApplyVenueUpdate(client_order_id, state, std::move(submit_result.venue_order_id));
}

[[nodiscard]] OrderAuditEventType AuditTypeFor(SubmitOutcome outcome) noexcept {
  switch (outcome) {
    case SubmitOutcome::kOpen:
      return OrderAuditEventType::kVenueOpen;
    case SubmitOutcome::kFilled:
      return OrderAuditEventType::kVenueFilled;
    case SubmitOutcome::kRejected:
      return OrderAuditEventType::kVenueRejected;
    case SubmitOutcome::kUnknown:
      return OrderAuditEventType::kVenueUnknown;
  }
  return OrderAuditEventType::kVenueUnknown;
}

[[nodiscard]] bool IsFreshSnapshot(risk::MonotonicClock::time_point received_at, risk::MonotonicClock::time_point now,
                                   std::chrono::milliseconds max_age) noexcept {
  if (max_age <= std::chrono::milliseconds::zero()) {
    return false;
  }

  const auto age = now - received_at;

  return age >= risk::MonotonicClock::duration::zero() && age <= max_age;
}

[[nodiscard]] SubmissionResult RejectFor(risk::RejectReason reason) {
  risk::RiskDecision decision;
  decision.AddReason(reason);

  return SubmissionResult{
      .status = SubmissionStatus::kRiskRejected,
      .risk_decision = decision,
      .submit_outcome = std::nullopt,
  };
}

}  // namespace

OrderSubmissionCoordinator::OrderSubmissionCoordinator(risk::RiskEngine& risk_engine, StartupRecoverySession recovery,
                                                       OrderSubmitter& submitter, MarketDataFreshnessProvider& market_data_freshness,
                                                       TradingDayProvider& trading_day_provider,
                                                       TradingDayContext trading_day_context) noexcept
    : recovery_(std::move(recovery)),
      order_gate_(risk_engine, recovery_.Registry(), risk::GateState::kReady),
      risk_engine_(risk_engine),
      submitter_(submitter),
      market_data_freshness_(market_data_freshness),
      trading_day_provider_(trading_day_provider),
      trading_day_context_(trading_day_context),
      last_account_state_version_(recovery_.RecoveredAccountStateVersion()) {}

SubmissionResult OrderSubmissionCoordinator::Execute(const risk::OrderIntent& order) {
  return ExecuteAtForTesting(order, risk::MonotonicClock::now());
}

SubmissionResult OrderSubmissionCoordinator::ExecuteAtForTesting(const risk::OrderIntent& order, risk::MonotonicClock::time_point now) {
  // Global execution-capability failures intentionally take priority over
  // intent-specific diagnostics such as DUPLICATE_CLIENT_ORDER_ID.
  const auto execution_day = trading_day_provider_.get().CurrentUtcDay();
  if (!execution_day || trading_day_context_.utc_day < 0 || trading_day_context_.baseline_equity.micros < 0 ||
      *execution_day != trading_day_context_.utc_day) {
    order_gate_.RequireReconciliation();
    return RejectFor(risk::RejectReason::kReconciliationRequired);
  }

  // Preserve identity/readiness priority before consulting the
  // authoritative account-state provider.
  //
  // In particular, a known client_order_id must still be reported as
  // DUPLICATE even if account state is currently unavailable.
  if (const auto precheck = order_gate_.Precheck(order)) {
    return SubmissionResult{
        .status = SubmissionStatus::kRiskRejected,
        .risk_decision = *precheck,
        .submit_outcome = std::nullopt,
    };
  }

  if (submitter_.get().Preflight(order) != VenuePreflightResult::kSupported) {
    return SubmissionResult{
        .status = SubmissionStatus::kVenuePreflightRejected,
        .risk_decision = {},
        .submit_outcome = std::nullopt,
    };
  }

  const auto market_data_cursor = market_data_freshness_.get().Current(order.market_id);
  if (!market_data_cursor || market_data_cursor->market_id != order.market_id || market_data_cursor->version == 0) {
    return RejectFor(risk::RejectReason::kMarketDataUnavailable);
  }

  // The caller owns economic intent, but never freshness evidence.
  auto trusted_order = order;
  trusted_order.market_data_received_at = market_data_cursor->received_at;

  auto& account_state_provider = recovery_.AccountStateProvider();

  const auto snapshot = account_state_provider.Snapshot(order.market_id);

  if (!snapshot) {
    return RejectFor(risk::RejectReason::kAccountStateUnavailable);
  }

  const auto account_state_policy = recovery_.AccountStatePolicy();

  if (!IsFreshSnapshot(snapshot->received_at, now, account_state_policy.max_age)) {
    return RejectFor(risk::RejectReason::kStaleAccountState);
  }

  // Never allow the authoritative stream to move backwards relative
  // to the startup baseline or a snapshot already observed by this
  // coordinator.
  if (snapshot->version < last_account_state_version_) {
    return RejectFor(risk::RejectReason::kAccountStateVersionRegressed);
  }

  last_account_state_version_ = snapshot->version;

  auto decision = order_gate_.CheckAndReserve(trusted_order, snapshot->state, now);

  if (!decision.Allowed()) {
    return SubmissionResult{
        .status = SubmissionStatus::kRiskRejected,
        .risk_decision = decision,
        .submit_outcome = std::nullopt,
    };
  }

  auto& registry = recovery_.Registry();
  auto& state_store = recovery_.StateStore();

  // Critical durability barrier:
  //
  // No external submit may begin until PENDING_SUBMIT identity and
  // reservation are committed durably.
  if (!PersistCurrentOrderState(state_store, registry, order.client_order_id, OrderAuditEventType::kIntentPendingDurable)) {
    // The durable claim never existed and Submit() has not started, so
    // undo the in-memory reservation and identity. Retrying this same
    // economic intent is safe.
    if (!registry.RollbackUnpersistedIntent(order.client_order_id)) {
      order_gate_.RequireReconciliation();

      return SubmissionResult{
          .status = SubmissionStatus::kRegistryTransitionFailed,
          .risk_decision = decision,
          .submit_outcome = std::nullopt,
      };
    }

    return SubmissionResult{
        .status = SubmissionStatus::kPreSubmitPersistenceFailed,
        .risk_decision = decision,
        .submit_outcome = std::nullopt,
    };
  }

  const auto abort_before_submit = [&](risk::RejectReason reason) {
    decision.AddReason(reason);
    if (!registry.MarkAbortedBeforeSubmit(order.client_order_id)) {
      order_gate_.RequireReconciliation();

      return SubmissionResult{
          .status = SubmissionStatus::kRegistryTransitionFailed,
          .risk_decision = decision,
          .submit_outcome = std::nullopt,
      };
    }

    if (!PersistCurrentOrderState(state_store, registry, order.client_order_id, OrderAuditEventType::kAbortedBeforeSubmit)) {
      order_gate_.RequireReconciliation();

      return SubmissionResult{
          .status = SubmissionStatus::kPreSubmitAbortPersistenceFailed,
          .risk_decision = decision,
          .submit_outcome = std::nullopt,
      };
    }

    return SubmissionResult{
        .status = SubmissionStatus::kAbortedBeforeSubmit,
        .risk_decision = decision,
        .submit_outcome = std::nullopt,
    };
  };

  // Close the potentially long persistence window immediately before the
  // external side effect. No caller-provided clock participates here.
  if (risk_engine_.get().KillSwitchActive()) {
    return abort_before_submit(risk::RejectReason::kKillSwitchActive);
  }

  const auto pre_submit_now = risk::MonotonicClock::now();
  const auto current_market_data = market_data_freshness_.get().Current(order.market_id);
  if (!current_market_data || current_market_data->market_id != order.market_id || current_market_data->version == 0) {
    return abort_before_submit(risk::RejectReason::kMarketDataUnavailable);
  }
  if (current_market_data->version != market_data_cursor->version || current_market_data->received_at != market_data_cursor->received_at) {
    return abort_before_submit(risk::RejectReason::kMarketDataChangedBeforeSubmit);
  }
  if (!IsFreshSnapshot(current_market_data->received_at, pre_submit_now, risk_engine_.get().Limits().max_market_data_age)) {
    return abort_before_submit(risk::RejectReason::kStaleMarketData);
  }

  const auto current_account_state = account_state_provider.Snapshot(order.market_id);
  if (!current_account_state) {
    return abort_before_submit(risk::RejectReason::kAccountStateUnavailable);
  }
  if (current_account_state->version != snapshot->version || current_account_state->received_at != snapshot->received_at ||
      current_account_state->state.market_gross_exposure != snapshot->state.market_gross_exposure ||
      current_account_state->state.total_gross_exposure != snapshot->state.total_gross_exposure ||
      current_account_state->state.pnl_since_baseline != snapshot->state.pnl_since_baseline) {
    return abort_before_submit(risk::RejectReason::kAccountStateChangedBeforeSubmit);
  }
  if (!IsFreshSnapshot(current_account_state->received_at, pre_submit_now, account_state_policy.max_age)) {
    return abort_before_submit(risk::RejectReason::kStaleAccountState);
  }

  // This is the final in-process check before the external side effect. A day
  // change after durable PENDING_SUBMIT requires a fresh recovery capability.
  const auto pre_submit_day = trading_day_provider_.get().CurrentUtcDay();
  if (!pre_submit_day || *pre_submit_day != trading_day_context_.utc_day) {
    order_gate_.RequireReconciliation();
    return abort_before_submit(risk::RejectReason::kReconciliationRequired);
  }

  // External side-effect boundary.
  const auto submit_result = submitter_.get().Submit(trusted_order);
  const auto outcome = submit_result.outcome;

  if (!ApplySubmitOutcome(registry, order.client_order_id, std::move(submit_result))) {
    order_gate_.RequireReconciliation();

    return SubmissionResult{
        .status = SubmissionStatus::kRegistryTransitionFailed,
        .risk_decision = decision,
        .submit_outcome = outcome,
    };
  }

  if (!PersistCurrentOrderState(state_store, registry, order.client_order_id, AuditTypeFor(outcome))) {
    // Runtime state is newer than durable state after an external
    // side effect. Do not retry Submit(). Fail closed instead.
    order_gate_.RequireReconciliation();

    return SubmissionResult{
        .status = SubmissionStatus::kPostSubmitPersistenceFailed,
        .risk_decision = decision,
        .submit_outcome = outcome,
    };
  }

  return SubmissionResult{
      .status = SubmissionStatus::kSubmitted,
      .risk_decision = decision,
      .submit_outcome = outcome,
  };
}

risk::GateState OrderSubmissionCoordinator::GateState() const noexcept {
  return order_gate_.State();
}

}  // namespace botguard::execution
