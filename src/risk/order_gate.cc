#include "order_gate.h"

#include <limits>
#include <optional>

namespace botguard::risk {
namespace {

[[nodiscard]] constexpr std::optional<Money> CheckedAddExposure(Money lhs, Money rhs) noexcept {
  if (lhs.micros < 0 || rhs.micros < 0) {
    return std::nullopt;
  }

  if (lhs.micros > std::numeric_limits<std::int64_t>::max() - rhs.micros) {
    return std::nullopt;
  }

  return Money{
      .micros = lhs.micros + rhs.micros,
  };
}

}  // namespace

OrderGate::OrderGate(RiskEngine& risk_engine, OrderRegistry& order_registry, GateState initial_state) noexcept
    : risk_engine_(risk_engine), order_registry_(order_registry), state_(initial_state) {}

std::optional<RiskDecision> OrderGate::Precheck(const OrderIntent& order) const {
  const auto& registry = order_registry_.get();

  // Identity has priority even while reconciliation is required.
  if (const auto reserved_notional = registry.ReservedNotional(order.client_order_id)) {
    RiskDecision decision;

    decision.SetOrderNotional(*reserved_notional);

    decision.AddReason(RejectReason::kDuplicateClientOrderId);

    return decision;
  }

  if (state_ != GateState::kReady) {
    RiskDecision decision;

    decision.AddReason(RejectReason::kReconciliationRequired);

    return decision;
  }

  return std::nullopt;
}

RiskDecision OrderGate::CheckAndReserve(const OrderIntent& order, const AccountState& state, MonotonicClock::time_point now) {
  auto& registry = order_registry_.get();

  if (const auto precheck = Precheck(order)) {
    return *precheck;
  }

  const auto effective_market_exposure = CheckedAddExposure(state.market_gross_exposure, registry.MarketReservedExposure(order.market_id));

  const auto effective_total_exposure = CheckedAddExposure(state.total_gross_exposure, registry.TotalReservedExposure());

  if (!effective_market_exposure || !effective_total_exposure) {
    auto decision = risk_engine_.get().Evaluate(order, state, now);

    if (!effective_market_exposure && !decision.HasReason(RejectReason::kMaxMarketExposureExceeded)) {
      decision.AddReason(RejectReason::kMaxMarketExposureExceeded);
    }

    if (!effective_total_exposure && !decision.HasReason(RejectReason::kMaxTotalExposureExceeded)) {
      decision.AddReason(RejectReason::kMaxTotalExposureExceeded);
    }

    return decision;
  }

  auto effective_state = state;
  effective_state.market_gross_exposure = *effective_market_exposure;
  effective_state.total_gross_exposure = *effective_total_exposure;

  auto decision = risk_engine_.get().Evaluate(order, effective_state, now);

  if (!decision.Allowed()) {
    return decision;
  }

  if (!registry.BeginIntent(order, decision.OrderNotional())) {
    // Under the single risk-executor invariant this should only be
    // reachable if the intent was unexpectedly claimed between the
    // check above and this point. Fail closed.
    decision.AddReason(RejectReason::kDuplicateClientOrderId);
  }

  return decision;
}

void OrderGate::RequireReconciliation() noexcept {
  state_ = GateState::kReconciliationRequired;
}

GateState OrderGate::State() const noexcept {
  return state_;
}

}  // namespace botguard::risk
