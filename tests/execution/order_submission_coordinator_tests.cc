#include "execution/authoritative_account_state_provider.h"
#include "execution/order_submission_coordinator.h"
#include "execution/reconciliation_coordinator.h"
#include "execution/startup_recovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace execution = botguard::execution;
namespace risk = botguard::risk;

enum class Step {
  kUpsert,
  kSubmit,
};

class FakeOrderStateStore final : public execution::OrderStateStore {
 public:
  explicit FakeOrderStateStore(std::vector<Step>& steps) noexcept : steps_(steps) {}

  void FailOnUpsertCall(std::size_t call_number) noexcept { fail_on_upsert_call_ = call_number; }

  void FailLoad(bool fail) noexcept { fail_load_ = fail; }

  void ActivateKillSwitchAfterUpsert(std::size_t call_number, risk::RiskEngine& engine) noexcept {
    activate_kill_switch_on_call_ = call_number;
    kill_switch_engine_ = &engine;
  }

  void AfterUpsert(std::size_t call_number, std::function<void()> callback) {
    callback_on_call_ = call_number;
    after_upsert_ = std::move(callback);
  }

  [[nodiscard]] bool PersistTransition(const risk::OrderRegistryEntry& entry, execution::OrderAuditEventType event_type) noexcept override {
    ++upsert_count_;

    steps_.get().push_back(Step::kUpsert);

    if (fail_on_upsert_call_.has_value() && upsert_count_ == *fail_on_upsert_call_) {
      // Atomic failure semantics:
      // the previously committed value remains unchanged.
      return false;
    }

    committed_.insert_or_assign(entry.client_order_id, entry);
    events_.push_back(execution::OrderAuditEvent{
        .sequence = events_.size() + 1,
        .client_order_id = entry.client_order_id,
        .type = event_type,
        .resulting_state = entry.state,
        .venue_order_id = entry.venue_order_id,
    });

    if (activate_kill_switch_on_call_.has_value() && upsert_count_ == *activate_kill_switch_on_call_ && kill_switch_engine_ != nullptr) {
      kill_switch_engine_->SetKillSwitch(true);
    }

    if (callback_on_call_.has_value() && upsert_count_ == *callback_on_call_ && after_upsert_) {
      after_upsert_();
    }

    return true;
  }

  [[nodiscard]] bool LoadAll(std::vector<risk::OrderRegistryEntry>& entries) noexcept override {
    if (fail_load_) {
      return false;
    }

    std::vector<risk::OrderRegistryEntry> loaded;
    loaded.reserve(committed_.size());

    for (const auto& [client_order_id, entry] : committed_) {
      static_cast<void>(client_order_id);
      loaded.push_back(entry);
    }

    entries.swap(loaded);

    return true;
  }

  [[nodiscard]] bool LoadAuditEvents(std::vector<execution::OrderAuditEvent>& events) noexcept override {
    events = events_;
    return true;
  }

  [[nodiscard]] std::size_t UpsertCount() const noexcept { return upsert_count_; }

  [[nodiscard]] std::size_t CommittedCount() const noexcept { return committed_.size(); }

  [[nodiscard]]
  std::optional<risk::OrderRegistryEntry> CommittedEntry(risk::ClientOrderId client_order_id) const {
    const auto iterator = committed_.find(client_order_id);

    if (iterator == committed_.end()) {
      return std::nullopt;
    }

    return iterator->second;
  }

 private:
  std::reference_wrapper<std::vector<Step>> steps_;

  std::optional<std::size_t> fail_on_upsert_call_;

  bool fail_load_{};
  std::size_t upsert_count_{};

  std::unordered_map<risk::ClientOrderId, risk::OrderRegistryEntry> committed_;
  std::vector<execution::OrderAuditEvent> events_;

  std::optional<std::size_t> activate_kill_switch_on_call_;

  risk::RiskEngine* kill_switch_engine_{};
  std::optional<std::size_t> callback_on_call_;
  std::function<void()> after_upsert_;
};

class FakeOrderSubmitter final : public execution::OrderSubmitter {
 public:
  FakeOrderSubmitter(execution::SubmitOutcome outcome, std::vector<Step>& steps) noexcept : outcome_(outcome), steps_(steps) {}

  [[nodiscard]] execution::VenuePreflightResult Preflight(const risk::OrderIntent&) const noexcept override {
    ++preflight_count_;
    return preflight_result_;
  }

  [[nodiscard]] execution::VenueSubmitResult Submit(const risk::OrderIntent& /*order*/) noexcept override {
    ++submit_count_;

    steps_.get().push_back(Step::kSubmit);

    return execution::VenueSubmitResult{.outcome = outcome_};
  }

  [[nodiscard]] std::size_t SubmitCount() const noexcept { return submit_count_; }

  [[nodiscard]] std::size_t PreflightCount() const noexcept { return preflight_count_; }

  void SetPreflightResult(execution::VenuePreflightResult result) noexcept { preflight_result_ = result; }

 private:
  execution::SubmitOutcome outcome_;

  std::reference_wrapper<std::vector<Step>> steps_;

  std::size_t submit_count_{};

  mutable std::size_t preflight_count_{};

  execution::VenuePreflightResult preflight_result_{execution::VenuePreflightResult::kSupported};
};

class FakeAuthoritativeAccountStateProvider final : public execution::AuthoritativeAccountStateProvider {
 public:
  FakeAuthoritativeAccountStateProvider(risk::MonotonicClock::time_point received_at, risk::AccountState state) noexcept
      : cursor_{
            .version =
                execution::AccountStateVersion{
                    .value = 1,
                },
            .received_at = received_at,
        },
        snapshot_{
            .state = state,
            .version =
                execution::AccountStateVersion{
                    .value = 1,
                },
            .received_at = received_at,
        } {}

  [[nodiscard]]
  std::optional<execution::AccountStateCursor> CurrentCursor() const noexcept override {
    if (!cursor_available_) {
      return std::nullopt;
    }

    return cursor_;
  }

  [[nodiscard]]
  std::optional<execution::AccountStateSnapshot> Snapshot(risk::MarketId /*market_id*/) const noexcept override {
    if (!snapshot_available_) {
      return std::nullopt;
    }

    return snapshot_;
  }

  void SetCursorAvailable(bool available) noexcept { cursor_available_ = available; }

  void SetSnapshotAvailable(bool available) noexcept { snapshot_available_ = available; }

  void SetCursorVersion(std::uint64_t version) noexcept { cursor_.version.value = version; }

  void SetSnapshotVersion(std::uint64_t version) noexcept { snapshot_.version.value = version; }

  void SetCursorReceivedAt(risk::MonotonicClock::time_point received_at) noexcept { cursor_.received_at = received_at; }

  void SetSnapshotReceivedAt(risk::MonotonicClock::time_point received_at) noexcept { snapshot_.received_at = received_at; }

  void SetState(risk::AccountState state) noexcept { snapshot_.state = state; }

 private:
  bool cursor_available_{true};
  bool snapshot_available_{true};

  execution::AccountStateCursor cursor_;
  execution::AccountStateSnapshot snapshot_;
};

class FakeMarketDataFreshnessProvider final : public execution::MarketDataFreshnessProvider {
 public:
  void Set(risk::MonotonicClock::time_point received_at, bool available = true, risk::MarketId market_id = 10) noexcept {
    available_ = available;
    cursor_ = execution::MarketDataCursor{.market_id = market_id, .version = ++version_, .received_at = received_at};
  }

  [[nodiscard]] std::optional<execution::MarketDataCursor> Current(risk::MarketId /*market_id*/) const noexcept override {
    return available_ ? std::optional<execution::MarketDataCursor>{cursor_} : std::nullopt;
  }

 private:
  bool available_{true};
  std::uint64_t version_{};
  execution::MarketDataCursor cursor_{};
};

[[nodiscard]] execution::MarketDataFreshnessProvider& MarketDataAt(risk::MonotonicClock::time_point received_at) noexcept {
  static thread_local FakeMarketDataFreshnessProvider provider;
  provider.Set(received_at);
  return provider;
}

class MutableTradingDayProvider final : public execution::TradingDayProvider {
 public:
  [[nodiscard]] std::optional<std::int64_t> CurrentUtcDay() noexcept override { return day; }

  std::optional<std::int64_t> day{20'000};
};

[[nodiscard]] MutableTradingDayProvider& TradingDay() noexcept {
  static thread_local MutableTradingDayProvider provider;
  provider.day = 20'000;
  return provider;
}

[[nodiscard]] execution::TradingDayContext TradingContext() noexcept {
  return execution::TradingDayContext{.utc_day = 20'000, .baseline_equity = risk::Money::FromWholeUsd(1'000)};
}

class FakeStartupReconciler final : public execution::StartupReconciler {
 public:
  [[nodiscard]] bool Reconcile(execution::ReconciliationCoordinator&
                               /*reconciliation*/) noexcept override {
    ++reconcile_count_;
    return succeed_;
  }

  void SetSuccess(bool succeed) noexcept { succeed_ = succeed; }

  [[nodiscard]] std::size_t ReconcileCount() const noexcept { return reconcile_count_; }

 private:
  bool succeed_{true};
  std::size_t reconcile_count_{};
};

// Represents an authoritative venue result that all unresolved
// startup orders were definitively rejected.
class RejectUnresolvedStartupReconciler final : public execution::StartupReconciler {
 public:
  [[nodiscard]] bool Reconcile(execution::ReconciliationCoordinator& reconciliation) noexcept override {
    const auto snapshot = reconciliation.Snapshot();

    for (const auto& entry : snapshot) {
      if (entry.state != risk::OrderState::kPendingSubmit && entry.state != risk::OrderState::kUnknown) {
        continue;
      }

      const auto result = reconciliation.MarkRejected(entry.client_order_id);

      if (result != execution::ReconciliationUpdateStatus::kApplied) {
        return false;
      }
    }

    return true;
  }
};

// Deliberately buggy adapter:
// ignores failed reconciliation mutations/persistence and returns
// success. ReconciliationCoordinator::Healthy() must still prevent
// creation of a trading capability.
class IgnoreUpdateFailureStartupReconciler final : public execution::StartupReconciler {
 public:
  [[nodiscard]] bool Reconcile(execution::ReconciliationCoordinator& reconciliation) noexcept override {
    const auto snapshot = reconciliation.Snapshot();

    for (const auto& entry : snapshot) {
      if (entry.state == risk::OrderState::kPendingSubmit || entry.state == risk::OrderState::kUnknown) {
        static_cast<void>(reconciliation.MarkRejected(entry.client_order_id));
      }
    }

    return true;
  }
};

risk::RiskLimits DefaultLimits() {
  return risk::RiskLimits{
      .max_order_notional = risk::Money::FromWholeUsd(100),
      .max_market_gross_exposure = risk::Money::FromWholeUsd(500),
      .max_total_gross_exposure = risk::Money::FromWholeUsd(2'000),
      .max_loss_since_baseline = risk::Money::FromWholeUsd(250),
      .max_market_data_age = 1s,
  };
}

risk::OrderIntent ValidOrder(risk::MonotonicClock::time_point now) {
  return risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(50),
      .quantity = risk::Quantity::FromWhole(100),
      .market_data_received_at = now,
  };
}

risk::AccountState SafeState() {
  return risk::AccountState{
      .market_gross_exposure = risk::Money::FromWholeUsd(100),
      .total_gross_exposure = risk::Money::FromWholeUsd(500),
      .pnl_since_baseline = risk::Money::FromWholeUsd(10),
  };
}

execution::AccountStateSafetyPolicy DefaultAccountStatePolicy() {
  return execution::AccountStateSafetyPolicy{
      .max_age = 1s,
  };
}

[[nodiscard]] auto RecoverForTrading(risk::OrderRegistry& registry, execution::OrderStateStore& store,
                                     execution::AuthoritativeAccountStateProvider& account_state_provider,
                                     risk::MonotonicClock::time_point now) {
  FakeStartupReconciler reconciler;

  return execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider, DefaultAccountStatePolicy(), now);
}

TEST(OrderSubmissionCoordinatorTest, UpsertsPendingBeforeSubmitAndFinalStateAfterSubmit) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kSubmitted);

  ASSERT_TRUE(result.submit_outcome.has_value());

  EXPECT_EQ(*result.submit_outcome, execution::SubmitOutcome::kOpen);

  ASSERT_EQ(steps.size(), 3U);

  EXPECT_EQ(steps[0], Step::kUpsert);

  EXPECT_EQ(steps[1], Step::kSubmit);

  EXPECT_EQ(steps[2], Step::kUpsert);

  EXPECT_EQ(store.UpsertCount(), 2U);

  EXPECT_EQ(store.CommittedCount(), 1U);

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kOpen);

  EXPECT_TRUE(committed->reservation_active);

  EXPECT_EQ(submitter.SubmitCount(), 1U);

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kOpen);
}

TEST(OrderSubmissionCoordinatorTest, UtcDayChangeAfterPendingCommitAbortsBeforeVenueSubmitAndRequiresReconciliation) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  FakeMarketDataFreshnessProvider market_data;
  market_data.Set(now);
  MutableTradingDayProvider day_provider;
  day_provider.day = 20'000;
  store.AfterUpsert(1, [&] { day_provider.day = 20'001; });

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, day_provider, TradingContext());

  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kAbortedBeforeSubmit);
  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kReconciliationRequired));
  EXPECT_EQ(submitter.SubmitCount(), 0U);
  EXPECT_EQ(coordinator.GateState(), risk::GateState::kReconciliationRequired);
  const auto committed = store.CommittedEntry(ValidOrder(now).client_order_id);
  ASSERT_TRUE(committed.has_value());
  EXPECT_EQ(committed->state, risk::OrderState::kAbortedBeforeSubmit);
}

TEST(OrderSubmissionCoordinatorTest, UtcDayMismatchAtExecuteBeginRejectsBeforeReservationOrVenueAccess) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  MutableTradingDayProvider day_provider;
  day_provider.day = 20'001;

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), day_provider,
                                                    TradingContext());

  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kReconciliationRequired));
  EXPECT_EQ(submitter.PreflightCount(), 0U);
  EXPECT_EQ(submitter.SubmitCount(), 0U);
  EXPECT_EQ(store.UpsertCount(), 0U);
  EXPECT_EQ(coordinator.GateState(), risk::GateState::kReconciliationRequired);
}

TEST(OrderSubmissionCoordinatorTest, VenuePreflightPrecedesTrustedInputsRiskReservationPersistenceAndSubmit) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  submitter.SetPreflightResult(execution::VenuePreflightResult::kUnsupported);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  account_state_provider.SetSnapshotAvailable(false);
  FakeMarketDataFreshnessProvider unavailable_market_data;
  unavailable_market_data.Set(now, false);
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, unavailable_market_data, TradingDay(),
                                                    TradingContext());
  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kVenuePreflightRejected);
  EXPECT_FALSE(result.submit_outcome.has_value());
  EXPECT_EQ(submitter.PreflightCount(), 1U);
  EXPECT_EQ(submitter.SubmitCount(), 0U);
  EXPECT_EQ(store.UpsertCount(), 0U);
  EXPECT_TRUE(steps.empty());
  EXPECT_FALSE(registry.Contains(order.client_order_id));
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});
}

TEST(OrderSubmissionCoordinatorTest, DuplicateIdentityPrecheckPrecedesVenuePreflight) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto order = ValidOrder(now);
  ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);
  ASSERT_EQ(submitter.PreflightCount(), 1U);
  ASSERT_EQ(submitter.SubmitCount(), 1U);

  submitter.SetPreflightResult(execution::VenuePreflightResult::kUnsupported);
  const auto retry = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(retry.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_TRUE(retry.risk_decision.HasReason(risk::RejectReason::kDuplicateClientOrderId));
  EXPECT_EQ(submitter.PreflightCount(), 1U);
  EXPECT_EQ(submitter.SubmitCount(), 1U);
}

TEST(OrderSubmissionCoordinatorTest, PreSubmitPersistenceFailureRollsBackReservation) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  store.FailOnUpsertCall(1);

  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kPreSubmitPersistenceFailed);

  EXPECT_FALSE(result.submit_outcome.has_value());

  EXPECT_EQ(store.UpsertCount(), 1U);

  EXPECT_EQ(store.CommittedCount(), 0U);

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  ASSERT_EQ(steps.size(), 1U);

  EXPECT_EQ(steps[0], Step::kUpsert);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  EXPECT_EQ(registry.MarketReservedExposure(order.market_id), risk::Money{});
}

TEST(OrderSubmissionCoordinatorTest, PreSubmitPersistenceFailureReleasesClientOrderId) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  store.FailOnUpsertCall(1);

  const auto order = ValidOrder(now);
  const auto result = coordinator.ExecuteAtForTesting(order, now);

  ASSERT_EQ(result.status, execution::SubmissionStatus::kPreSubmitPersistenceFailed);
  EXPECT_FALSE(registry.Contains(order.client_order_id));
  EXPECT_FALSE(registry.State(order.client_order_id).has_value());
}

TEST(OrderSubmissionCoordinatorTest, SameIntentCanRetryAfterPreSubmitPersistenceFailure) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  store.FailOnUpsertCall(1);

  const auto order = ValidOrder(now);

  const auto first = coordinator.ExecuteAtForTesting(order, now);
  ASSERT_EQ(first.status, execution::SubmissionStatus::kPreSubmitPersistenceFailed);
  ASSERT_EQ(submitter.SubmitCount(), 0U);

  const auto second = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(second.status, execution::SubmissionStatus::kSubmitted);
  EXPECT_EQ(submitter.SubmitCount(), 1U);
  EXPECT_EQ(store.UpsertCount(), 3U);
  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kOpen);
}

TEST(OrderSubmissionCoordinatorTest, UpsertsUnknownAfterAmbiguousSubmit) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kUnknown, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kSubmitted);

  EXPECT_EQ(store.UpsertCount(), 2U);

  EXPECT_EQ(store.CommittedCount(), 1U);

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kUnknown);

  EXPECT_TRUE(committed->reservation_active);

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
}

TEST(OrderSubmissionCoordinatorTest, UpsertsRejectedStateAndReleasesReservation) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kRejected, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kSubmitted);

  EXPECT_EQ(store.CommittedCount(), 1U);

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kRejected);

  EXPECT_FALSE(committed->reservation_active);

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kRejected);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});
}

TEST(OrderSubmissionCoordinatorTest, PostSubmitPersistenceFailureKeepsDurablePendingFallback) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  store.FailOnUpsertCall(2);

  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kPostSubmitPersistenceFailed);

  EXPECT_EQ(submitter.SubmitCount(), 1U);

  EXPECT_EQ(store.UpsertCount(), 2U);

  ASSERT_EQ(steps.size(), 3U);

  EXPECT_EQ(steps[0], Step::kUpsert);

  EXPECT_EQ(steps[1], Step::kSubmit);

  EXPECT_EQ(steps[2], Step::kUpsert);

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kOpen);

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kPendingSubmit);

  EXPECT_TRUE(committed->reservation_active);

  EXPECT_EQ(coordinator.GateState(), risk::GateState::kReconciliationRequired);

  // Verify reconciliation-required has priority over provider failure.
  account_state_provider.SetSnapshotAvailable(false);

  auto second_order = order;
  second_order.client_order_id = 102;

  const auto second_result = coordinator.ExecuteAtForTesting(second_order, now);

  EXPECT_EQ(second_result.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(second_result.risk_decision.HasReason(risk::RejectReason::kReconciliationRequired));

  EXPECT_FALSE(second_result.risk_decision.HasReason(risk::RejectReason::kAccountStateUnavailable));

  EXPECT_EQ(submitter.SubmitCount(), 1U);
}

TEST(OrderSubmissionCoordinatorTest, RiskRejectedIntentNeverUpsertsOrSubmits) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  auto order = ValidOrder(now);

  order.price = risk::Price{};

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kInvalidPrice));

  EXPECT_EQ(store.UpsertCount(), 0U);

  EXPECT_EQ(store.CommittedCount(), 0U);

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  EXPECT_TRUE(steps.empty());

  EXPECT_FALSE(registry.Contains(order.client_order_id));
}

TEST(OrderSubmissionCoordinatorTest, FilledOrderRemainsReservedUntilExposureReconciliation) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kFilled, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kSubmitted);

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kFilled);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kFilled);

  EXPECT_TRUE(committed->reservation_active);
}

TEST(OrderSubmissionCoordinatorTest, LoadAllReturnsOnlyLatestCommittedStatePerIntent) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto order = ValidOrder(now);

  ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);

  std::vector<risk::OrderRegistryEntry> loaded;

  ASSERT_TRUE(store.LoadAll(loaded));

  ASSERT_EQ(loaded.size(), 1U);

  EXPECT_EQ(loaded.front().client_order_id, order.client_order_id);

  EXPECT_EQ(loaded.front().state, risk::OrderState::kOpen);

  EXPECT_TRUE(loaded.front().reservation_active);

  risk::OrderRegistry restored;

  EXPECT_TRUE(restored.Restore(loaded));

  EXPECT_EQ(restored.State(order.client_order_id), risk::OrderState::kOpen);

  EXPECT_EQ(restored.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
}

TEST(OrderSubmissionCoordinatorTest, DurableUnknownPreventsStartupRecoveryFromProducingTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  FakeOrderStateStore store(steps);

  const auto order = ValidOrder(now);

  {
    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;

    FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

    FakeOrderSubmitter submitter(execution::SubmitOutcome::kUnknown, steps);

    auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);

    ASSERT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);
  }

  risk::OrderRegistry second_registry;

  FakeStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider second_account_state_provider(now, SafeState());

  auto second_recovery = execution::StartupRecoverySession::Recover(second_registry, store, reconciler, second_account_state_provider,
                                                                    DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(second_recovery.has_value());

  EXPECT_EQ(second_recovery.error(), execution::StartupRecoveryError::kUnresolvedOrdersRemain);

  EXPECT_EQ(second_registry.State(order.client_order_id), risk::OrderState::kUnknown);

  EXPECT_EQ(second_registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
}

TEST(OrderSubmissionCoordinatorTest, ReconciledDurableUnknownAllowsStartupRecoveryAfterRestart) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  FakeOrderStateStore store(steps);

  const auto order = ValidOrder(now);

  {
    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;

    FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

    FakeOrderSubmitter submitter(execution::SubmitOutcome::kUnknown, steps);

    auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);

    ASSERT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);
  }

  risk::OrderRegistry second_registry;

  RejectUnresolvedStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider second_account_state_provider(now, SafeState());

  auto recovery = execution::StartupRecoverySession::Recover(second_registry, store, reconciler, second_account_state_provider,
                                                             DefaultAccountStatePolicy(), now);

  ASSERT_TRUE(recovery.has_value());

  EXPECT_EQ(second_registry.State(order.client_order_id), risk::OrderState::kRejected);

  EXPECT_EQ(second_registry.TotalReservedExposure(), risk::Money{});

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kRejected);

  EXPECT_FALSE(committed->reservation_active);

  risk::RiskEngine second_engine(DefaultLimits());

  std::vector<Step> second_steps;

  FakeOrderSubmitter second_submitter(execution::SubmitOutcome::kOpen, second_steps);

  execution::OrderSubmissionCoordinator second_coordinator(second_engine, std::move(*recovery), second_submitter, MarketDataAt(now),
                                                           TradingDay(), TradingContext());
  EXPECT_EQ(second_coordinator.GateState(), risk::GateState::kReady);

  const auto retry = second_coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(retry.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(retry.risk_decision.HasReason(risk::RejectReason::kDuplicateClientOrderId));

  EXPECT_EQ(second_submitter.SubmitCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, StartupLoadFailureDoesNotProduceTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);
  store.FailLoad(true);

  FakeStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  auto recovery =
      execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider, DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(recovery.has_value());

  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kLoadFailed);

  EXPECT_EQ(reconciler.ReconcileCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, StartupReconcilerFailureDoesNotProduceTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeStartupReconciler reconciler;
  reconciler.SetSuccess(false);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  auto recovery =
      execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider, DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(recovery.has_value());

  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kReconciliationFailed);

  EXPECT_EQ(reconciler.ReconcileCount(), 1U);
}

TEST(OrderSubmissionCoordinatorTest, StartupReconciliationPersistenceFailureDoesNotProduceTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  FakeOrderStateStore store(steps);

  const auto order = ValidOrder(now);

  {
    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;

    FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

    FakeOrderSubmitter submitter(execution::SubmitOutcome::kUnknown, steps);

    auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);

    ASSERT_EQ(store.UpsertCount(), 2U);
  }

  store.FailOnUpsertCall(3);

  risk::OrderRegistry second_registry;

  RejectUnresolvedStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider second_account_state_provider(now, SafeState());

  auto recovery = execution::StartupRecoverySession::Recover(second_registry, store, reconciler, second_account_state_provider,
                                                             DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(recovery.has_value());

  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kReconciliationFailed);

  EXPECT_EQ(second_registry.State(order.client_order_id), risk::OrderState::kRejected);

  EXPECT_EQ(second_registry.TotalReservedExposure(), risk::Money{});

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kUnknown);

  EXPECT_TRUE(committed->reservation_active);
}

TEST(OrderSubmissionCoordinatorTest, IgnoredReconciliationPersistenceFailureCannotProduceTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  FakeOrderStateStore store(steps);

  const auto order = ValidOrder(now);

  {
    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;

    FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

    FakeOrderSubmitter submitter(execution::SubmitOutcome::kUnknown, steps);

    auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);
  }

  store.FailOnUpsertCall(3);

  risk::OrderRegistry restored_registry;

  IgnoreUpdateFailureStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider second_account_state_provider(now, SafeState());

  auto recovery = execution::StartupRecoverySession::Recover(restored_registry, store, reconciler, second_account_state_provider,
                                                             DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(recovery.has_value());

  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kReconciliationFailed);

  EXPECT_EQ(restored_registry.State(order.client_order_id), risk::OrderState::kRejected);

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kUnknown);

  EXPECT_TRUE(committed->reservation_active);
}

TEST(OrderSubmissionCoordinatorTest, KillSwitchActivatedAfterPendingUpsertPreventsSubmit) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  store.ActivateKillSwitchAfterUpsert(1, engine);

  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kAbortedBeforeSubmit);

  EXPECT_FALSE(result.submit_outcome.has_value());

  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kKillSwitchActive));

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  ASSERT_EQ(steps.size(), 2U);

  EXPECT_EQ(steps[0], Step::kUpsert);

  EXPECT_EQ(steps[1], Step::kUpsert);

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kAbortedBeforeSubmit);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kAbortedBeforeSubmit);

  EXPECT_FALSE(committed->reservation_active);
}

TEST(OrderSubmissionCoordinatorTest, MarketDataBecomingStaleDuringPersistenceAbortsDurablyBeforeSubmit) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  auto limits = DefaultLimits();
  limits.max_market_data_age = 1ms;
  risk::RiskEngine engine(limits);
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  FakeMarketDataFreshnessProvider market_data;
  market_data.Set(now);
  store.AfterUpsert(1, [] { std::this_thread::sleep_for(5ms); });
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, TradingDay(), TradingContext());
  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kAbortedBeforeSubmit);
  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kStaleMarketData));
  EXPECT_EQ(submitter.SubmitCount(), 0U);
  EXPECT_EQ(registry.State(101), risk::OrderState::kAbortedBeforeSubmit);
  ASSERT_TRUE(store.CommittedEntry(101).has_value());
  EXPECT_EQ(store.CommittedEntry(101)->state, risk::OrderState::kAbortedBeforeSubmit);
}

TEST(OrderSubmissionCoordinatorTest, AccountStateBecomingStaleDuringPersistenceAbortsDurablyBeforeSubmit) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  FakeStartupReconciler reconciler;
  auto recovery = execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider,
                                                             execution::AccountStateSafetyPolicy{.max_age = 1ms}, now);
  ASSERT_TRUE(recovery.has_value());
  FakeMarketDataFreshnessProvider market_data;
  market_data.Set(now);
  store.AfterUpsert(1, [] { std::this_thread::sleep_for(5ms); });
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, TradingDay(), TradingContext());
  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kAbortedBeforeSubmit);
  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kStaleAccountState));
  EXPECT_EQ(submitter.SubmitCount(), 0U);
  EXPECT_EQ(registry.State(101), risk::OrderState::kAbortedBeforeSubmit);
}

TEST(OrderSubmissionCoordinatorTest, MarketDataRevisionChangeDuringPersistenceAbortsBeforeSubmit) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  FakeMarketDataFreshnessProvider market_data;
  market_data.Set(now);
  store.AfterUpsert(1, [&] { market_data.Set(risk::MonotonicClock::now()); });
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, TradingDay(), TradingContext());
  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kAbortedBeforeSubmit);
  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kMarketDataChangedBeforeSubmit));
  EXPECT_EQ(submitter.SubmitCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, AccountRevisionChangeDuringPersistenceAbortsBeforeSubmit) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  FakeMarketDataFreshnessProvider market_data;
  market_data.Set(now);
  store.AfterUpsert(1, [&] { account_state_provider.SetSnapshotVersion(2); });
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, TradingDay(), TradingContext());
  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kAbortedBeforeSubmit);
  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kAccountStateChangedBeforeSubmit));
  EXPECT_EQ(submitter.SubmitCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, AbortPersistenceFailureRequiresReconciliation) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  store.ActivateKillSwitchAfterUpsert(1, engine);

  store.FailOnUpsertCall(2);

  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kPreSubmitAbortPersistenceFailed);

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  EXPECT_EQ(coordinator.GateState(), risk::GateState::kReconciliationRequired);

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kAbortedBeforeSubmit);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  const auto committed = store.CommittedEntry(order.client_order_id);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kPendingSubmit);

  EXPECT_TRUE(committed->reservation_active);
}

TEST(OrderSubmissionCoordinatorTest, AccountStateUnavailableRejectsWithoutReserveOrSubmit) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  account_state_provider.SetSnapshotAvailable(false);

  const auto order = ValidOrder(now);

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kAccountStateUnavailable));

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  EXPECT_EQ(store.UpsertCount(), 0U);

  EXPECT_FALSE(registry.Contains(order.client_order_id));
}

TEST(OrderSubmissionCoordinatorTest, StaleAccountStateRejectsWithoutReserveOrSubmit) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  account_state_provider.SetSnapshotReceivedAt(now - 2s);

  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kStaleAccountState));

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  EXPECT_EQ(store.UpsertCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, FutureDatedAccountStateRejectsWithoutReserveOrSubmit) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  account_state_provider.SetSnapshotReceivedAt(now + 1ms);

  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kStaleAccountState));

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  EXPECT_EQ(store.UpsertCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, AccountStateVersionRegressionRejectsWithoutSubmit) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  account_state_provider.SetCursorVersion(10);
  account_state_provider.SetSnapshotVersion(10);

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  account_state_provider.SetSnapshotVersion(9);

  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kAccountStateVersionRegressed));

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  EXPECT_EQ(store.UpsertCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, SameAccountStateVersionMayBeReusedWhileFresh) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  account_state_provider.SetCursorVersion(10);
  account_state_provider.SetSnapshotVersion(10);

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kRejected, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  auto first_order = ValidOrder(now);

  ASSERT_EQ(coordinator.ExecuteAtForTesting(first_order, now).status, execution::SubmissionStatus::kSubmitted);

  auto second_order = ValidOrder(now);

  second_order.client_order_id = 102;

  const auto second_result = coordinator.ExecuteAtForTesting(second_order, now);

  EXPECT_EQ(second_result.status, execution::SubmissionStatus::kSubmitted);

  EXPECT_EQ(submitter.SubmitCount(), 2U);
}

TEST(OrderSubmissionCoordinatorTest, NewerAccountStateVersionIsAccepted) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  account_state_provider.SetCursorVersion(10);
  account_state_provider.SetSnapshotVersion(10);

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kRejected, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  ASSERT_EQ(coordinator.ExecuteAtForTesting(ValidOrder(now), now).status, execution::SubmissionStatus::kSubmitted);

  account_state_provider.SetSnapshotVersion(11);

  auto second_order = ValidOrder(now);

  second_order.client_order_id = 102;

  const auto result = coordinator.ExecuteAtForTesting(second_order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kSubmitted);

  EXPECT_EQ(submitter.SubmitCount(), 2U);
}

TEST(OrderSubmissionCoordinatorTest, AuthoritativeAccountStateDrivesRiskDecision) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  auto account_state = SafeState();

  account_state.total_gross_exposure = risk::Money::FromWholeUsd(1'980);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, account_state);

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kMaxTotalExposureExceeded));

  EXPECT_EQ(submitter.SubmitCount(), 0U);

  EXPECT_EQ(store.UpsertCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, DuplicateIntentHasPriorityOverUnavailableAccountState) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);

  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto order = ValidOrder(now);

  ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);

  ASSERT_EQ(submitter.SubmitCount(), 1U);

  account_state_provider.SetSnapshotAvailable(false);

  const auto retry = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(retry.status, execution::SubmissionStatus::kRiskRejected);

  EXPECT_TRUE(retry.risk_decision.HasReason(risk::RejectReason::kDuplicateClientOrderId));

  EXPECT_FALSE(retry.risk_decision.HasReason(risk::RejectReason::kAccountStateUnavailable));

  EXPECT_EQ(submitter.SubmitCount(), 1U);
}

TEST(OrderSubmissionCoordinatorTest, CallerCannotDeclareStaleMarketDataFresh) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  FakeMarketDataFreshnessProvider market_data;
  market_data.Set(now - 2s);
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, TradingDay(), TradingContext());
  auto order = ValidOrder(now);
  order.market_data_received_at = now;

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kStaleMarketData));
  EXPECT_EQ(submitter.SubmitCount(), 0U);
  EXPECT_EQ(store.UpsertCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, TrustedMarketDataOverridesCallerTimestamp) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  FakeMarketDataFreshnessProvider market_data;
  market_data.Set(now);
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, TradingDay(), TradingContext());
  auto order = ValidOrder(now);
  order.market_data_received_at = now - 1h;

  const auto result = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kSubmitted);
  EXPECT_EQ(submitter.SubmitCount(), 1U);
}

TEST(OrderSubmissionCoordinatorTest, UnavailableTrustedMarketDataRejectsWithoutPersistenceOrSubmit) {
  const auto now = risk::MonotonicClock::now();
  std::vector<Step> steps;
  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FakeOrderStateStore store(steps);
  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());
  FakeOrderSubmitter submitter(execution::SubmitOutcome::kOpen, steps);
  auto recovery = RecoverForTrading(registry, store, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  FakeMarketDataFreshnessProvider market_data;
  market_data.Set(now, false);
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, TradingDay(), TradingContext());
  const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

  EXPECT_EQ(result.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_TRUE(result.risk_decision.HasReason(risk::RejectReason::kMarketDataUnavailable));
  EXPECT_EQ(submitter.SubmitCount(), 0U);
  EXPECT_EQ(store.UpsertCount(), 0U);
}

TEST(OrderSubmissionCoordinatorTest, StartupAccountStateUnavailableDoesNotProduceTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  account_state_provider.SetCursorAvailable(false);

  auto recovery =
      execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider, DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(recovery.has_value());

  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kAccountStateUnavailable);
}

TEST(OrderSubmissionCoordinatorTest, StartupStaleAccountStateDoesNotProduceTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider account_state_provider(now - 2s, SafeState());

  auto recovery =
      execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider, DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(recovery.has_value());

  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kStaleAccountState);
}

TEST(OrderSubmissionCoordinatorTest, StartupFutureDatedAccountStateDoesNotProduceTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider account_state_provider(now + 1ms, SafeState());

  auto recovery =
      execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider, DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(recovery.has_value());

  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kStaleAccountState);
}

TEST(OrderSubmissionCoordinatorTest, StartupZeroAccountStateVersionDoesNotProduceTradingCapability) {
  const auto now = risk::MonotonicClock::now();

  std::vector<Step> steps;

  risk::OrderRegistry registry;

  FakeOrderStateStore store(steps);

  FakeStartupReconciler reconciler;

  FakeAuthoritativeAccountStateProvider account_state_provider(now, SafeState());

  account_state_provider.SetCursorVersion(0);

  auto recovery =
      execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider, DefaultAccountStatePolicy(), now);

  ASSERT_FALSE(recovery.has_value());

  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kInvalidAccountState);
}

}  // namespace
