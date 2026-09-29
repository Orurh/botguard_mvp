#pragma once

#include "order_registry.h"
#include "risk_engine.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace botguard::risk {

enum class GateState : std::uint8_t {
  kReconciliationRequired,
  kReady,
};

// Not thread-safe.
// All methods must be called from the owning risk-executor thread.
class OrderGate {
 public:
  OrderGate(
      RiskEngine& risk_engine,
      OrderRegistry& order_registry,
      GateState initial_state) noexcept;

  // Read-only admission precheck.
  //
  // Returns a rejection when identity/readiness alone is sufficient
  // to reject the intent. std::nullopt means authoritative account
  // state is required for the remaining risk evaluation.
  [[nodiscard]] std::optional<RiskDecision>
  Precheck(const OrderIntent& order) const;

  [[nodiscard]] RiskDecision CheckAndReserve(
      const OrderIntent& order,
      const AccountState& state,
      MonotonicClock::time_point now);

  void RequireReconciliation() noexcept;

  [[nodiscard]] GateState State() const noexcept;

 private:
  std::reference_wrapper<RiskEngine> risk_engine_;
  std::reference_wrapper<OrderRegistry> order_registry_;
  GateState state_;
};

}  // namespace botguard::risk