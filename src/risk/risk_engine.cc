#include "risk_engine.h"
#include "order_notional.h"

#include <limits>
#include <optional>

namespace botguard::risk {
namespace {

[[nodiscard]] constexpr std::optional<std::int64_t> CheckedAddNonNegative(std::int64_t lhs, std::int64_t rhs) noexcept {
  if (lhs < 0 || rhs < 0) {
    return std::nullopt;
  }

  if (lhs > std::numeric_limits<std::int64_t>::max() - rhs) {
    return std::nullopt;
  }

  return lhs + rhs;
}

[[nodiscard]] constexpr bool ProjectedExposureExceeds(Money current, Money added, Money limit) noexcept {
  const auto projected = CheckedAddNonNegative(current.micros, added.micros);

  return !projected || *projected > limit.micros;
}

}  // namespace

RiskEngine::RiskEngine(RiskLimits limits) : limits_(limits) {}

RiskDecision RiskEngine::Evaluate(const OrderIntent& order, const AccountState& state, MonotonicClock::time_point now) const noexcept {
  RiskDecision decision;

  if (kill_switch_active_.load(std::memory_order_relaxed)) {
    decision.AddReason(RejectReason::kKillSwitchActive);
  }

  const bool valid_price = order.price.micros_per_unit > 0;

  const bool valid_quantity = order.quantity.microunits > 0;

  if (!valid_price) {
    decision.AddReason(RejectReason::kInvalidPrice);
  }

  if (!valid_quantity) {
    decision.AddReason(RejectReason::kInvalidQuantity);
  }

  const auto market_data_age = now - order.market_data_received_at;

  if (market_data_age < MonotonicClock::duration::zero() || market_data_age > limits_.max_market_data_age) {
    decision.AddReason(RejectReason::kStaleMarketData);
  }

  if (valid_price && valid_quantity) {
    const auto notional = ComputeOrderNotional(order.price, order.quantity);

    if (!notional) {
      decision.AddReason(RejectReason::kNotionalOverflow);
    } else {
      decision.SetOrderNotional(*notional);

      if (*notional > limits_.max_order_notional) {
        decision.AddReason(RejectReason::kMaxOrderNotionalExceeded);
      }

      if (ProjectedExposureExceeds(state.market_gross_exposure, *notional, limits_.max_market_gross_exposure)) {
        decision.AddReason(RejectReason::kMaxMarketExposureExceeded);
      }

      if (ProjectedExposureExceeds(state.total_gross_exposure, *notional, limits_.max_total_gross_exposure)) {
        decision.AddReason(RejectReason::kMaxTotalExposureExceeded);
      }
    }
  }

  const auto max_loss_since_baseline = limits_.max_loss_since_baseline.micros;

  if (max_loss_since_baseline <= 0 || state.pnl_since_baseline.micros <= -max_loss_since_baseline) {
    decision.AddReason(RejectReason::kMaxLossSinceBaselineExceeded);
  }

  decision.AllowIfNoRejections();
  return decision;
}

void RiskEngine::SetKillSwitch(bool active) noexcept {
  kill_switch_active_.store(active, std::memory_order_relaxed);
}

bool RiskEngine::KillSwitchActive() const noexcept {
  return kill_switch_active_.load(std::memory_order_relaxed);
}

const RiskLimits& RiskEngine::Limits() const noexcept {
  return limits_;
}

const char* ToString(RejectReason reason) noexcept {
  switch (reason) {
    case RejectReason::kKillSwitchActive:
      return "KILL_SWITCH_ACTIVE";

    case RejectReason::kInvalidPrice:
      return "INVALID_PRICE";

    case RejectReason::kInvalidQuantity:
      return "INVALID_QUANTITY";

    case RejectReason::kDuplicateClientOrderId:
      return "DUPLICATE_CLIENT_ORDER_ID";

    case RejectReason::kReconciliationRequired:
      return "RECONCILIATION_REQUIRED";

    case RejectReason::kAccountStateUnavailable:
      return "ACCOUNT_STATE_UNAVAILABLE";

    case RejectReason::kStaleAccountState:
      return "STALE_ACCOUNT_STATE";

    case RejectReason::kAccountStateVersionRegressed:
      return "ACCOUNT_STATE_VERSION_REGRESSED";

    case RejectReason::kAccountStateChangedBeforeSubmit:
      return "ACCOUNT_STATE_CHANGED_BEFORE_SUBMIT";

    case RejectReason::kMarketDataUnavailable:
      return "MARKET_DATA_UNAVAILABLE";

    case RejectReason::kMarketDataChangedBeforeSubmit:
      return "MARKET_DATA_CHANGED_BEFORE_SUBMIT";

    case RejectReason::kStaleMarketData:
      return "STALE_MARKET_DATA";

    case RejectReason::kNotionalOverflow:
      return "NOTIONAL_OVERFLOW";

    case RejectReason::kMaxOrderNotionalExceeded:
      return "MAX_ORDER_NOTIONAL_EXCEEDED";

    case RejectReason::kMaxMarketExposureExceeded:
      return "MAX_MARKET_EXPOSURE_EXCEEDED";

    case RejectReason::kMaxTotalExposureExceeded:
      return "MAX_TOTAL_EXPOSURE_EXCEEDED";

    case RejectReason::kMaxLossSinceBaselineExceeded:
      return "MAX_LOSS_SINCE_BASELINE_EXCEEDED";

    case RejectReason::kCount:
      break;
  }

  return "UNKNOWN";
}

}  // namespace botguard::risk
