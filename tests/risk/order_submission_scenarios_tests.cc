#include "risk/order_gate.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace {

using namespace std::chrono_literals;
namespace risk = botguard::risk;

enum class FakeSubmitOutcome : std::uint8_t {
  kAccepted,
  kFilled,
  kRejected,
  kTimeout,
};

class FakeExchange {
 public:
  explicit FakeExchange(FakeSubmitOutcome outcome) noexcept : outcome_(outcome) {}

  [[nodiscard]] FakeSubmitOutcome Submit(const risk::OrderIntent& /*unused*/) noexcept {
    ++submit_count_;
    return outcome_;
  }

  [[nodiscard]] std::size_t SubmitCount() const noexcept { return submit_count_; }

 private:
  FakeSubmitOutcome outcome_;
  std::size_t submit_count_{};
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

bool ApplySubmitOutcome(risk::OrderRegistry& registry, risk::ClientOrderId client_order_id, FakeSubmitOutcome outcome) {
  switch (outcome) {
    case FakeSubmitOutcome::kAccepted:
      return registry.MarkOpen(client_order_id);

    case FakeSubmitOutcome::kFilled:
      return registry.MarkFilled(client_order_id);

    case FakeSubmitOutcome::kRejected:
      return registry.MarkRejected(client_order_id);

    case FakeSubmitOutcome::kTimeout:
      return registry.MarkUnknown(client_order_id);
  }

  return false;
}

TEST(OrderSubmissionScenarioTest, AcceptedSubmitBecomesOpen) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);
  FakeExchange exchange(FakeSubmitOutcome::kAccepted);

  const auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  ASSERT_TRUE(ApplySubmitOutcome(registry, order.client_order_id, exchange.Submit(order)));

  EXPECT_EQ(exchange.SubmitCount(), 1U);
  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kOpen);
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
}

TEST(OrderSubmissionScenarioTest, ImmediateFillBecomesFilled) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);
  FakeExchange exchange(FakeSubmitOutcome::kFilled);

  const auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  ASSERT_TRUE(ApplySubmitOutcome(registry, order.client_order_id, exchange.Submit(order)));

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kFilled);
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
}

TEST(OrderSubmissionScenarioTest, DefinitiveRejectBecomesRejected) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);
  FakeExchange exchange(FakeSubmitOutcome::kRejected);

  const auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  ASSERT_TRUE(ApplySubmitOutcome(registry, order.client_order_id, exchange.Submit(order)));

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kRejected);
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});
  EXPECT_FALSE(gate.CheckAndReserve(order, SafeState(), now).Allowed());
}

TEST(OrderSubmissionScenarioTest, TimeoutBecomesUnknownAndRetryDoesNotSubmit) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);
  FakeExchange exchange(FakeSubmitOutcome::kTimeout);

  const auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  ASSERT_TRUE(ApplySubmitOutcome(registry, order.client_order_id, exchange.Submit(order)));

  ASSERT_EQ(exchange.SubmitCount(), 1U);
  ASSERT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);

  const auto retry = gate.CheckAndReserve(order, SafeState(), now);

  EXPECT_FALSE(retry.Allowed());
  EXPECT_TRUE(retry.HasReason(risk::RejectReason::kDuplicateClientOrderId));
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
  // Critical invariant:
  // retry never reaches the exchange.
  EXPECT_EQ(exchange.SubmitCount(), 1U);
}

TEST(OrderSubmissionScenarioTest, UnknownCanReconcileToOpen) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);
  FakeExchange exchange(FakeSubmitOutcome::kTimeout);

  const auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  ASSERT_TRUE(ApplySubmitOutcome(registry, order.client_order_id, exchange.Submit(order)));

  ASSERT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);

  // Reconciliation later finds the order at the exchange.
  ASSERT_TRUE(registry.MarkOpen(order.client_order_id));

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kOpen);
}

TEST(OrderSubmissionScenarioTest, UnknownCanReconcileToRejected) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);
  FakeExchange exchange(FakeSubmitOutcome::kTimeout);

  const auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  ASSERT_TRUE(ApplySubmitOutcome(registry, order.client_order_id, exchange.Submit(order)));

  ASSERT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);

  // Reconciliation proves that the exchange did not accept it.
  ASSERT_TRUE(registry.MarkRejected(order.client_order_id));

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kRejected);

  // Same identity is still immutable.
  EXPECT_FALSE(gate.CheckAndReserve(order, SafeState(), now).Allowed());
}

TEST(OrderSubmissionScenarioTest, RiskRejectedIntentNeverReachesExchange) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);
  FakeExchange exchange(FakeSubmitOutcome::kAccepted);

  auto order = ValidOrder(now);
  order.price = risk::Price{};

  const auto decision = gate.CheckAndReserve(order, SafeState(), now);

  ASSERT_FALSE(decision.Allowed());

  // Do not call Submit() after a rejected admission decision.
  EXPECT_EQ(exchange.SubmitCount(), 0U);
  EXPECT_FALSE(registry.Contains(order.client_order_id));
}

TEST(OrderSubmissionScenarioTest, FilledExposureMovesFromLocalReservationToAccountStateWithoutDoubleCounting) {
  const auto now = risk::MonotonicClock::now();

  auto limits = DefaultLimits();
  limits.max_total_gross_exposure = risk::Money::FromWholeUsd(100);
  limits.max_market_gross_exposure = risk::Money::FromWholeUsd(100);

  risk::RiskEngine engine(limits);
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);

  FakeExchange exchange(FakeSubmitOutcome::kFilled);

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{};

  auto first = ValidOrder(now);
  first.client_order_id = 101;
  first.price = risk::Price::FromCents(60);
  first.quantity = risk::Quantity::FromWhole(100);

  ASSERT_TRUE(gate.CheckAndReserve(first, state, now).Allowed());

  ASSERT_TRUE(ApplySubmitOutcome(registry, first.client_order_id, exchange.Submit(first)));

  ASSERT_EQ(registry.State(first.client_order_id), risk::OrderState::kFilled);

  ASSERT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(60));

  // Exchange reconciliation now reports the filled position.
  state.market_gross_exposure = risk::Money::FromWholeUsd(60);
  state.total_gross_exposure = risk::Money::FromWholeUsd(60);

  // The authoritative AccountState now contains this exposure,
  // so the local reservation must be released.
  ASSERT_TRUE(registry.MarkExposureReconciled(first.client_order_id));

  ASSERT_EQ(registry.TotalReservedExposure(), risk::Money{});

  auto second = ValidOrder(now);
  second.client_order_id = 102;
  second.price = risk::Price::FromCents(40);
  second.quantity = risk::Quantity::FromWhole(100);

  const auto second_decision = gate.CheckAndReserve(second, state, now);

  // 60 authoritative + 40 newly reserved = exactly 100.
  EXPECT_TRUE(second_decision.Allowed());

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(40));
}

TEST(OrderSubmissionScenarioTest, FilledReservationRemainsConservativeUntilExplicitReconciliation) {
  const auto now = risk::MonotonicClock::now();

  auto limits = DefaultLimits();
  limits.max_total_gross_exposure = risk::Money::FromWholeUsd(100);

  risk::RiskEngine engine(limits);
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{};

  auto first = ValidOrder(now);
  first.client_order_id = 101;
  first.price = risk::Price::FromCents(60);
  first.quantity = risk::Quantity::FromWhole(100);

  ASSERT_TRUE(gate.CheckAndReserve(first, state, now).Allowed());

  ASSERT_TRUE(registry.MarkFilled(101));

  // Account snapshot has advanced, but reservation has not yet been
  // explicitly reconciled.
  state.market_gross_exposure = risk::Money::FromWholeUsd(60);
  state.total_gross_exposure = risk::Money::FromWholeUsd(60);

  auto second = first;
  second.client_order_id = 102;
  second.price = risk::Price::FromCents(40);

  const auto decision = gate.CheckAndReserve(second, state, now);

  // Fail closed until reconciliation resolves the overlap.
  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxTotalExposureExceeded));
}

TEST(OrderSubmissionScenarioTest, UnknownOrderSurvivesRestartAndRetryStillDoesNotSubmit) {
  const auto now = risk::MonotonicClock::now();

  const auto order = ValidOrder(now);

  FakeExchange exchange(FakeSubmitOutcome::kTimeout);

  risk::RiskEngine first_engine(DefaultLimits());

  risk::OrderRegistry first_registry;

  risk::OrderGate first_gate(first_engine, first_registry, risk::GateState::kReady);

  ASSERT_TRUE(first_gate.CheckAndReserve(order, SafeState(), now).Allowed());

  ASSERT_TRUE(ApplySubmitOutcome(first_registry, order.client_order_id, exchange.Submit(order)));

  ASSERT_EQ(first_registry.State(order.client_order_id), risk::OrderState::kUnknown);

  ASSERT_EQ(exchange.SubmitCount(), 1U);

  const auto persisted_state = first_registry.Snapshot();

  // ---- simulated process restart ----

  risk::RiskEngine second_engine(DefaultLimits());

  risk::OrderRegistry second_registry;

  ASSERT_TRUE(second_registry.Restore(persisted_state));

  risk::OrderGate second_gate(second_engine, second_registry, risk::GateState::kReconciliationRequired);

  ASSERT_EQ(second_gate.State(), risk::GateState::kReconciliationRequired);

  ASSERT_EQ(second_registry.State(order.client_order_id), risk::OrderState::kUnknown);

  ASSERT_EQ(second_registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));

  // Retrying the same intent remains a duplicate even while
  // startup reconciliation is required.
  const auto retry = second_gate.CheckAndReserve(order, SafeState(), now);

  EXPECT_FALSE(retry.Allowed());

  EXPECT_TRUE(retry.HasReason(risk::RejectReason::kDuplicateClientOrderId));

  EXPECT_FALSE(retry.HasReason(risk::RejectReason::kReconciliationRequired));

  // Retry never reaches the exchange.
  EXPECT_EQ(exchange.SubmitCount(), 1U);

  EXPECT_EQ(second_registry.State(order.client_order_id), risk::OrderState::kUnknown);

  // A genuinely new economic intent must fail closed until
  // startup reconciliation completes.
  auto new_order = order;
  new_order.client_order_id = 102;

  const auto new_intent_before_reconciliation = second_gate.CheckAndReserve(new_order, SafeState(), now);

  EXPECT_FALSE(new_intent_before_reconciliation.Allowed());

  EXPECT_TRUE(new_intent_before_reconciliation.HasReason(risk::RejectReason::kReconciliationRequired));

  EXPECT_FALSE(new_intent_before_reconciliation.HasReason(risk::RejectReason::kDuplicateClientOrderId));

  EXPECT_FALSE(second_registry.Contains(new_order.client_order_id));

  // Authoritative reconciliation proves that the timed-out order
  // was not accepted by the exchange.
  ASSERT_TRUE(second_registry.MarkRejected(order.client_order_id));

  EXPECT_EQ(second_registry.TotalReservedExposure(), risk::Money{});

  // Important invariant:
  // an existing fail-closed gate cannot transition back to READY.
  EXPECT_EQ(second_gate.State(), risk::GateState::kReconciliationRequired);

  // Successful recovery creates a new execution/gate lifetime.
  risk::OrderGate recovered_gate(second_engine, second_registry, risk::GateState::kReady);

  ASSERT_EQ(recovered_gate.State(), risk::GateState::kReady);

  const auto new_intent_after_reconciliation = recovered_gate.CheckAndReserve(new_order, SafeState(), now);

  EXPECT_TRUE(new_intent_after_reconciliation.Allowed());

  EXPECT_TRUE(second_registry.Contains(new_order.client_order_id));

  EXPECT_EQ(second_registry.State(new_order.client_order_id), risk::OrderState::kPendingSubmit);

  // No additional exchange submission happened inside this test.
  EXPECT_EQ(exchange.SubmitCount(), 1U);
}

}  // namespace