#include "risk/risk_engine.h"

#include <chrono>
#include <cstdint>
#include <iostream>

int main() {
  namespace risk = botguard::risk;
  using namespace std::chrono_literals;

  risk::RiskLimits limits;
  limits.max_order_notional = risk::Money::FromWholeUsd(100);
  limits.max_market_gross_exposure = risk::Money::FromWholeUsd(500);
  limits.max_total_gross_exposure = risk::Money::FromWholeUsd(2'000);
  limits.max_loss_since_baseline = risk::Money::FromWholeUsd(250);
  limits.max_market_data_age = 1s;

  risk::RiskEngine engine(limits);
  const auto now = risk::MonotonicClock::now();

  risk::OrderIntent order{
      .strategy_id = 1,
      .client_order_id = 1,
      .market_id = 1,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(42),
      .quantity = risk::Quantity::FromWhole(100),
      .market_data_received_at = now,
  };

  const risk::AccountState state{
      .market_gross_exposure = risk::Money::FromWholeUsd(200),
      .total_gross_exposure = risk::Money::FromWholeUsd(600),
      .pnl_since_baseline = risk::Money::FromWholeUsd(-20),
  };

  constexpr std::uint64_t k_iterations = 5'000'000;
  std::uint64_t checksum = 0;

  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < k_iterations; ++i) {
    order.client_order_id = i + 1;
    const auto decision = engine.Evaluate(order, state, now);
    checksum += static_cast<std::uint64_t>(decision.OrderNotional().micros);
    checksum += decision.Allowed() ? 1U : 0U;
  }
  const auto finish = std::chrono::steady_clock::now();

  const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
  const auto ns_per_call = static_cast<double>(elapsed_ns) / static_cast<double>(k_iterations);

  std::cout << "iterations=" << k_iterations << '\n' << "ns/evaluate=" << ns_per_call << '\n' << "checksum=" << checksum << '\n';
}
