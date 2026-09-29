#include "risk/order_gate.h"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using namespace std::chrono_literals;
namespace risk = botguard::risk;

risk::RiskLimits DefaultLimits() {
  risk::RiskLimits limits;
  limits.max_order_notional = risk::Money::FromWholeUsd(100);
  limits.max_market_gross_exposure = risk::Money::FromWholeUsd(500);
  limits.max_total_gross_exposure = risk::Money::FromWholeUsd(2'000);
  limits.max_loss_since_baseline = risk::Money::FromWholeUsd(250);
  limits.max_market_data_age = 1s;
  return limits;
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

TEST(OrderGateTest, AllowedOrderIsReservedBeforeSubmit) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);

  const auto order = ValidOrder(now);

  const auto decision = gate.CheckAndReserve(order, SafeState(), now);

  ASSERT_TRUE(decision.Allowed());

  ASSERT_TRUE(registry.State(order.client_order_id).has_value());
  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kPendingSubmit);
}

TEST(OrderGateTest, RejectsDuplicateReservedIntent) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);

  const auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  const auto retry = gate.CheckAndReserve(order, SafeState(), now);

  EXPECT_FALSE(retry.Allowed());
  EXPECT_TRUE(retry.HasReason(risk::RejectReason::kDuplicateClientOrderId));

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kPendingSubmit);
}

TEST(OrderGateTest, RiskRejectedOrderIsNotReserved) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);

  auto order = ValidOrder(now);
  order.price = risk::Price{};

  const auto decision = gate.CheckAndReserve(order, SafeState(), now);

  ASSERT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kInvalidPrice));

  EXPECT_FALSE(registry.Contains(order.client_order_id));
}

TEST(OrderGateTest, UnknownIntentCannotBeReservedAgain) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);

  const auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  ASSERT_TRUE(registry.MarkUnknown(order.client_order_id));

  const auto retry = gate.CheckAndReserve(order, SafeState(), now);

  EXPECT_FALSE(retry.Allowed());
  EXPECT_TRUE(retry.HasReason(risk::RejectReason::kDuplicateClientOrderId));

  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);
}

TEST(OrderGateTest, PendingReservationsCountTowardTotalExposure) {
  const auto now = risk::MonotonicClock::now();

  auto limits = DefaultLimits();
  limits.max_total_gross_exposure = risk::Money::FromWholeUsd(100);
  limits.max_market_gross_exposure = risk::Money::FromWholeUsd(500);

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

  auto second = first;
  second.client_order_id = 102;

  ASSERT_TRUE(gate.CheckAndReserve(first, state, now).Allowed());

  const auto second_decision = gate.CheckAndReserve(second, state, now);

  EXPECT_FALSE(second_decision.Allowed());
  EXPECT_TRUE(second_decision.HasReason(risk::RejectReason::kMaxTotalExposureExceeded));

  EXPECT_FALSE(registry.Contains(102));
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(60));
}

TEST(OrderGateTest, PendingReservationsCountTowardMarketExposure) {
  const auto now = risk::MonotonicClock::now();

  auto limits = DefaultLimits();
  limits.max_market_gross_exposure = risk::Money::FromWholeUsd(100);
  limits.max_total_gross_exposure = risk::Money::FromWholeUsd(500);

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

  auto second = first;
  second.client_order_id = 102;

  ASSERT_TRUE(gate.CheckAndReserve(first, state, now).Allowed());

  const auto second_decision = gate.CheckAndReserve(second, state, now);

  EXPECT_FALSE(second_decision.Allowed());
  EXPECT_TRUE(second_decision.HasReason(risk::RejectReason::kMaxMarketExposureExceeded));

  EXPECT_FALSE(registry.Contains(102));
}

TEST(OrderGateTest, UnknownOrderContinuesToConsumeRiskCapacity) {
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

  ASSERT_TRUE(registry.MarkUnknown(101));

  auto second = first;
  second.client_order_id = 102;

  const auto decision = gate.CheckAndReserve(second, state, now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxTotalExposureExceeded));
}

TEST(OrderGateTest, RejectedOrderReleasesRiskCapacity) {
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

  ASSERT_TRUE(registry.MarkRejected(101));

  auto second = first;
  second.client_order_id = 102;

  EXPECT_TRUE(gate.CheckAndReserve(second, state, now).Allowed());
}

TEST(OrderGateTest, DuplicateIntentIsRejectedBeforeRiskReevaluation) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  risk::OrderGate gate(engine, registry, risk::GateState::kReady);

  auto order = ValidOrder(now);

  ASSERT_TRUE(gate.CheckAndReserve(order, SafeState(), now).Allowed());

  order.market_data_received_at = now - 2s;

  const auto retry = gate.CheckAndReserve(order, SafeState(), now);

  EXPECT_FALSE(retry.Allowed());
  EXPECT_TRUE(retry.HasReason(risk::RejectReason::kDuplicateClientOrderId));

  EXPECT_FALSE(retry.HasReason(risk::RejectReason::kStaleMarketData));

  EXPECT_EQ(retry.Reasons().size(), 1U);
}

TEST(OrderGateTest, RejectsNewIntentWhileReconciliationIsRequired) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  risk::OrderGate gate(engine, registry, risk::GateState::kReconciliationRequired);

  const auto order = ValidOrder(now);

  const auto decision = gate.CheckAndReserve(order, SafeState(), now);

  EXPECT_FALSE(decision.Allowed());

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kReconciliationRequired));

  EXPECT_FALSE(registry.Contains(order.client_order_id));

  EXPECT_EQ(gate.State(), risk::GateState::kReconciliationRequired);
}

TEST(OrderGateTest, KnownIntentRemainsDuplicateWhileReconciliationIsRequired) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  const auto order = ValidOrder(now);

  ASSERT_TRUE(registry.BeginIntent(order, risk::Money::FromWholeUsd(50)));

  ASSERT_TRUE(registry.MarkUnknown(order.client_order_id));

  risk::OrderGate gate(engine, registry, risk::GateState::kReconciliationRequired);

  const auto decision = gate.CheckAndReserve(order, SafeState(), now);

  EXPECT_FALSE(decision.Allowed());

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kDuplicateClientOrderId));

  EXPECT_FALSE(decision.HasReason(risk::RejectReason::kReconciliationRequired));

  EXPECT_EQ(decision.Reasons().size(), 1U);
}

TEST(OrderGateTest, RequireReconciliationMovesReadyGateFailClosed) {
  const auto now = risk::MonotonicClock::now();

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;

  risk::OrderGate gate(engine, registry, risk::GateState::kReady);

  EXPECT_EQ(gate.State(), risk::GateState::kReady);

  gate.RequireReconciliation();

  EXPECT_EQ(gate.State(), risk::GateState::kReconciliationRequired);

  const auto order = ValidOrder(now);

  const auto decision = gate.CheckAndReserve(order, SafeState(), now);

  EXPECT_FALSE(decision.Allowed());

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kReconciliationRequired));

  EXPECT_FALSE(registry.Contains(order.client_order_id));
}

}  // namespace
