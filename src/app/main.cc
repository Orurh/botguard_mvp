#include "risk/order_gate.h"

#include <chrono>
#include <iostream>

namespace {

namespace risk = botguard::risk;

void PrintDecision(const risk::RiskDecision& decision) {
  std::cout << (decision.Allowed() ? "ALLOW" : "REJECT") << " notional_micros=" << decision.OrderNotional().micros << '\n';

  for (const auto reason : decision.Reasons()) {
    std::cout << "  - " << risk::ToString(reason) << '\n';
  }
}

void PrintOrderState(const risk::OrderRegistry& registry, risk::ClientOrderId client_order_id) {
  const auto state = registry.State(client_order_id);

  if (!state) {
    std::cout << "order_state=NOT_REGISTERED\n";
    return;
  }

  std::cout << "order_state=" << risk::ToString(*state) << '\n';
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  const risk::RiskLimits limits{
      .max_order_notional = risk::Money::FromWholeUsd(100),
      .max_market_gross_exposure = risk::Money::FromWholeUsd(500),
      .max_total_gross_exposure = risk::Money::FromWholeUsd(2'000),
      .max_loss_since_baseline = risk::Money::FromWholeUsd(250),
      .max_market_data_age = 1s,
  };

  risk::RiskEngine risk_engine(limits);
  risk::OrderRegistry order_registry;
  risk::OrderGate order_gate(risk_engine, order_registry, risk::GateState::kReady);

  const auto now = risk::MonotonicClock::now();

  const risk::OrderIntent order{
      .strategy_id = 1,
      .client_order_id = 42,
      .market_id = 7,
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

  std::cout << "Initial intent:\n";

  const auto initial_decision = order_gate.CheckAndReserve(order, state, now);

  PrintDecision(initial_decision);
  PrintOrderState(order_registry, order.client_order_id);

  if (!initial_decision.Allowed()) {
    return 1;
  }

  std::cout << "\nSimulating exchange timeout...\n";

  if (!order_registry.MarkUnknown(order.client_order_id)) {
    std::cerr << "failed to transition order to UNKNOWN\n";
    return 1;
  }

  PrintOrderState(order_registry, order.client_order_id);

  std::cout << "\nRetrying same client_order_id:\n";

  const auto retry_decision = order_gate.CheckAndReserve(order, state, risk::MonotonicClock::now());

  PrintDecision(retry_decision);
  PrintOrderState(order_registry, order.client_order_id);

  if (retry_decision.Allowed()) {
    std::cerr << "SAFETY FAILURE: duplicate intent was allowed\n";
    return 1;
  }

  if (!retry_decision.HasReason(risk::RejectReason::kDuplicateClientOrderId)) {
    std::cerr << "SAFETY FAILURE: retry rejected for wrong reason\n";
    return 1;
  }

  if (order_registry.State(order.client_order_id) != risk::OrderState::kUnknown) {
    std::cerr << "SAFETY FAILURE: UNKNOWN state was lost\n";
    return 1;
  }

  std::cout << "\nBotGuard safety scenario passed.\n";
  return 0;
}
