#pragma once

#include "market_data_freshness_provider.h"
#include "order_submitter.h"
#include "startup_recovery.h"
#include "trading_day_context.h"
#include "trading_day_provider.h"

#include "risk/order_gate.h"
#include "risk/risk_engine.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace botguard::execution {

enum class SubmissionStatus : std::uint8_t {
  kRiskRejected,

  kVenuePreflightRejected,

  kPreSubmitPersistenceFailed,

  kAbortedBeforeSubmit,

  kPreSubmitAbortPersistenceFailed,

  kSubmitted,

  kPostSubmitPersistenceFailed,

  kRegistryTransitionFailed,
};

struct SubmissionResult {
  SubmissionStatus status{SubmissionStatus::kRiskRejected};

  risk::RiskDecision risk_decision{};

  std::optional<SubmitOutcome> submit_outcome;
};

// Can only be created from a successful StartupRecoverySession.
//
// The recovery capability binds this coordinator to exactly one:
// - OrderRegistry;
// - OrderStateStore;
// - authoritative account-state provider.
//
// AccountState is never accepted from strategy/caller code.
//
// Not thread-safe.
// Must be called from the owning execution thread.
class OrderSubmissionCoordinator {
 public:
  OrderSubmissionCoordinator(risk::RiskEngine& risk_engine, StartupRecoverySession recovery, OrderSubmitter& submitter,
                             MarketDataFreshnessProvider& market_data_freshness, TradingDayProvider& trading_day_provider,
                             TradingDayContext trading_day_context) noexcept;

  // Production entry point: caller cannot choose the evaluation clock.
  [[nodiscard]] SubmissionResult Execute(const risk::OrderIntent& order);

  // Deterministic core/testing entry point. The actual external side-effect
  // boundary always rechecks safety against an internally captured clock.
  [[nodiscard]] SubmissionResult ExecuteAtForTesting(const risk::OrderIntent& order, risk::MonotonicClock::time_point now);

  [[nodiscard]] risk::GateState GateState() const noexcept;

 private:
  StartupRecoverySession recovery_;

  risk::OrderGate order_gate_;

  std::reference_wrapper<risk::RiskEngine> risk_engine_;

  std::reference_wrapper<OrderSubmitter> submitter_;

  std::reference_wrapper<MarketDataFreshnessProvider> market_data_freshness_;

  std::reference_wrapper<TradingDayProvider> trading_day_provider_;

  TradingDayContext trading_day_context_;

  AccountStateVersion last_account_state_version_{};
};

}  // namespace botguard::execution
