#include "risk/risk_engine.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>

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

risk::RiskLimits WideLimits() {
  constexpr auto k_max = std::numeric_limits<std::int64_t>::max();

  risk::RiskLimits limits;
  limits.max_order_notional = risk::Money{.micros = k_max};
  limits.max_market_gross_exposure = risk::Money{.micros = k_max};
  limits.max_total_gross_exposure = risk::Money{.micros = k_max};
  limits.max_loss_since_baseline = risk::Money{.micros = k_max};
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

TEST(RiskEngineTest, AllowsSafeOrder) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());

  const auto decision = engine.Evaluate(ValidOrder(now), SafeState(), now);

  EXPECT_TRUE(decision.Allowed());
  EXPECT_EQ(decision.OrderNotional(), risk::Money::FromWholeUsd(50));
  EXPECT_TRUE(decision.Reasons().empty());
}

TEST(RiskEngineTest, SupportsFractionalQuantityWithoutFloatingPoint) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto order = ValidOrder(now);
  order.quantity = risk::Quantity{1'500'000};  // 1.5 units
  order.price = risk::Price::FromCents(40);

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{};

  const auto decision = engine.Evaluate(order, state, now);

  EXPECT_TRUE(decision.Allowed());
  EXPECT_EQ(decision.OrderNotional().micros, 600'000);  // $0.60
}

TEST(RiskEngineTest, RejectsStaleMarketData) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto order = ValidOrder(now);
  order.market_data_received_at = now - 1001ms;

  const auto decision = engine.Evaluate(order, SafeState(), now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kStaleMarketData));
}

TEST(RiskEngineTest, AcceptsMarketDataExactlyAtAgeLimit) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto order = ValidOrder(now);
  order.market_data_received_at = now - 1s;

  EXPECT_TRUE(engine.Evaluate(order, SafeState(), now).Allowed());
}

TEST(RiskEngineTest, RejectsFutureDatedMarketData) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto order = ValidOrder(now);
  order.market_data_received_at = now + 1ms;

  const auto decision = engine.Evaluate(order, SafeState(), now);

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kStaleMarketData));
}

TEST(RiskEngineTest, RejectsOrderAboveNotionalLimit) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto order = ValidOrder(now);
  order.price = risk::Price::FromCents(51);
  order.quantity = risk::Quantity::FromWhole(200);

  const auto decision = engine.Evaluate(order, SafeState(), now);

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxOrderNotionalExceeded));
}

TEST(RiskEngineTest, AllowsOrderExactlyAtNotionalLimit) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto order = ValidOrder(now);
  order.price = risk::Price::FromCents(50);
  order.quantity = risk::Quantity::FromWhole(200);

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{};

  EXPECT_TRUE(engine.Evaluate(order, state, now).Allowed());
}

TEST(RiskEngineTest, RejectsProjectedMarketExposureAboveLimit) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto state = SafeState();
  state.market_gross_exposure = risk::Money::FromWholeUsd(460);

  const auto decision = engine.Evaluate(ValidOrder(now), state, now);

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxMarketExposureExceeded));
}

TEST(RiskEngineTest, RejectsProjectedTotalExposureAboveLimit) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto state = SafeState();
  state.total_gross_exposure = risk::Money::FromWholeUsd(1'960);

  const auto decision = engine.Evaluate(ValidOrder(now), state, now);

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxTotalExposureExceeded));
}

TEST(RiskEngineTest, RejectsAtMaxLossSinceBaseline) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto state = SafeState();
  state.pnl_since_baseline = risk::Money::FromWholeUsd(-250);

  const auto decision = engine.Evaluate(ValidOrder(now), state, now);

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxLossSinceBaselineExceeded));
}

TEST(RiskEngineTest, RejectsWhenKillSwitchIsActive) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  engine.SetKillSwitch(true);

  const auto decision = engine.Evaluate(ValidOrder(now), SafeState(), now);

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kKillSwitchActive));
}

TEST(RiskEngineTest, RejectsZeroPrice) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());
  auto order = ValidOrder(now);
  order.price = risk::Price{};

  const auto decision = engine.Evaluate(order, SafeState(), now);

  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kInvalidPrice));
}

TEST(RiskEngineTest, RoundsFractionalNotionalUpToOneMoneyMicro) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());

  auto order = ValidOrder(now);
  order.price = risk::Price{.micros_per_unit = 1};
  order.quantity = risk::Quantity{.microunits = 1};

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{};

  const auto decision = engine.Evaluate(order, state, now);

  ASSERT_TRUE(decision.Allowed());
  EXPECT_EQ(decision.OrderNotional().micros, 1);
}

TEST(RiskEngineTest, DoesNotRejectWhenIntermediateProductOverflowsButFinalNotionalFits) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(WideLimits());

  constexpr auto k_max = std::numeric_limits<std::int64_t>::max();

  auto order = ValidOrder(now);
  order.price = risk::Price{
      .micros_per_unit = k_max,
  };
  order.quantity = risk::Quantity{
      .microunits = risk::kMicrounitsPerUnit - 1,
  };

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{};

  const auto decision = engine.Evaluate(order, state, now);

  ASSERT_TRUE(decision.Allowed());
  EXPECT_FALSE(decision.HasReason(risk::RejectReason::kNotionalOverflow));

  constexpr auto k_expected = k_max - (k_max / risk::kMicrounitsPerUnit);

  EXPECT_EQ(decision.OrderNotional().micros, k_expected);
}

TEST(RiskEngineTest, AllowsNotionalExactlyAtInt64Max) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(WideLimits());

  constexpr auto k_max = std::numeric_limits<std::int64_t>::max();

  auto order = ValidOrder(now);
  order.price = risk::Price{
      .micros_per_unit = k_max,
  };
  order.quantity = risk::Quantity{
      .microunits = risk::kMicrounitsPerUnit,
  };

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{};

  const auto decision = engine.Evaluate(order, state, now);

  ASSERT_TRUE(decision.Allowed());
  EXPECT_EQ(decision.OrderNotional().micros, k_max);
}

TEST(RiskEngineTest, RejectsWhenFinalNotionalOverflows) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(WideLimits());

  constexpr auto k_max = std::numeric_limits<std::int64_t>::max();

  auto order = ValidOrder(now);
  order.price = risk::Price{
      .micros_per_unit = k_max,
  };
  order.quantity = risk::Quantity{
      .microunits = risk::kMicrounitsPerUnit + 1,
  };

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{};

  const auto decision = engine.Evaluate(order, state, now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kNotionalOverflow));
}

TEST(RiskEngineTest, RejectsZeroQuantity) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());

  auto order = ValidOrder(now);
  order.quantity = risk::Quantity{};

  const auto decision = engine.Evaluate(order, SafeState(), now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kInvalidQuantity));
}

TEST(RiskEngineTest, RejectsNegativeQuantity) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());

  auto order = ValidOrder(now);
  order.quantity = risk::Quantity{
      .microunits = -1,
  };

  const auto decision = engine.Evaluate(order, SafeState(), now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kInvalidQuantity));
}

TEST(RiskEngineTest, RejectsNegativePrice) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());

  auto order = ValidOrder(now);
  order.price = risk::Price{
      .micros_per_unit = -1,
  };

  const auto decision = engine.Evaluate(order, SafeState(), now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kInvalidPrice));
}

TEST(RiskEngineTest, RejectsMarketExposureWhenProjectionOverflows) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(WideLimits());

  constexpr auto k_max = std::numeric_limits<std::int64_t>::max();

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{
      .micros = k_max - 1,
  };
  state.total_gross_exposure = risk::Money{};

  const auto decision = engine.Evaluate(ValidOrder(now), state, now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxMarketExposureExceeded));
}

TEST(RiskEngineTest, RejectsTotalExposureWhenProjectionOverflows) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(WideLimits());

  constexpr auto k_max = std::numeric_limits<std::int64_t>::max();

  auto state = SafeState();
  state.market_gross_exposure = risk::Money{};
  state.total_gross_exposure = risk::Money{
      .micros = k_max - 1,
  };

  const auto decision = engine.Evaluate(ValidOrder(now), state, now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxTotalExposureExceeded));
}

TEST(RiskEngineTest, ReportsMultipleRejectReasons) {
  const auto now = risk::MonotonicClock::now();
  risk::RiskEngine engine(DefaultLimits());

  auto order = ValidOrder(now);
  order.market_data_received_at = now - 2s;
  order.quantity = risk::Quantity::FromWhole(300);  // $150 > $100 limit

  engine.SetKillSwitch(true);

  const auto decision = engine.Evaluate(order, SafeState(), now);

  EXPECT_FALSE(decision.Allowed());
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kKillSwitchActive));
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kStaleMarketData));
  EXPECT_TRUE(decision.HasReason(risk::RejectReason::kMaxOrderNotionalExceeded));
  EXPECT_EQ(decision.Reasons().size(), 3U);
}

}  // namespace
