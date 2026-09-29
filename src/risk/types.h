#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace botguard::risk {

inline constexpr std::int64_t kMicrosPerUsd = 1'000'000;
inline constexpr std::int64_t kCentsPerUsd = 100;
inline constexpr std::int64_t kMicrosPerCent = kMicrosPerUsd / kCentsPerUsd;

inline constexpr std::int64_t kMicrounitsPerUnit = 1'000'000;

using StrategyId = std::uint64_t;
using ClientOrderId = std::uint64_t;
using MarketId = std::uint64_t;
using MonotonicClock = std::chrono::steady_clock;

struct Money {
  std::int64_t micros{};

  [[nodiscard]] static constexpr Money FromWholeUsd(std::int64_t usd) noexcept {
    return Money{
        .micros = usd * kMicrosPerUsd,
    };
  }

  auto operator<=>(const Money&) const = default;
};

struct Price {
  std::int64_t micros_per_unit{};

  [[nodiscard]] static constexpr Price FromCents(std::int64_t cents) noexcept {
    return Price{
        .micros_per_unit = cents * kMicrosPerCent,
    };
  }

  auto operator<=>(const Price&) const = default;
};

struct Quantity {
  std::int64_t microunits{};

  [[nodiscard]] static constexpr Quantity FromWhole(std::int64_t units) noexcept {
    return Quantity{
        .microunits = units * kMicrounitsPerUnit,
    };
  }

  auto operator<=>(const Quantity&) const = default;
};

enum class Side : std::uint8_t {
  kBuy,
  kSell,
};

enum class RiskVerdict : std::uint8_t {
  kAllow,
  kReject,
};

enum class RejectReason : std::uint8_t {
  kKillSwitchActive,
  kInvalidPrice,
  kInvalidQuantity,
  kDuplicateClientOrderId,
  kReconciliationRequired,
  kAccountStateUnavailable,
  kStaleAccountState,
  kAccountStateVersionRegressed,
  kAccountStateChangedBeforeSubmit,
  kMarketDataUnavailable,
  kMarketDataChangedBeforeSubmit,
  kStaleMarketData,

  kNotionalOverflow,
  kMaxOrderNotionalExceeded,
  kMaxMarketExposureExceeded,
  kMaxTotalExposureExceeded,
  kMaxLossSinceBaselineExceeded,

  kCount,
};

struct OrderIntent {
  StrategyId strategy_id{};
  ClientOrderId client_order_id{};
  MarketId market_id{};

  Side side{Side::kBuy};

  Price price{};
  Quantity quantity{};

  // Pure-core input only. Production execution boundaries must replace this
  // with BotGuard-owned MarketDataFreshnessProvider evidence.
  MonotonicClock::time_point market_data_received_at;
};

// Authoritative exposure already reflected in reconciled exchange/account state.
//
// Local unresolved order exposure (PENDING_SUBMIT / OPEN / UNKNOWN and
// unreconciled FILLED orders) is not included here. OrderGate adds local
// reservations before evaluating risk.
//
// Once a FILLED order is reflected in AccountState, its local reservation
// must be released via OrderRegistry::MarkExposureReconciled().
struct AccountState {
  Money market_gross_exposure{};
  Money total_gross_exposure{};
  // Current authoritative equity minus the durable per-day baseline. The
  // baseline is established on the first safe observation after rollover;
  // this is not calendar-day PnL unless a venue can prove boundary equity.
  Money pnl_since_baseline{};
};

struct RiskLimits {
  Money max_order_notional{};
  Money max_market_gross_exposure{};
  Money max_total_gross_exposure{};
  // Reject once pnl_since_baseline reaches the negative of this amount.
  Money max_loss_since_baseline{};

  std::chrono::milliseconds max_market_data_age{};
};

class RiskDecision {
 public:
  static constexpr std::size_t kMaxRejectReasons = static_cast<std::size_t>(RejectReason::kCount);

  [[nodiscard]] bool Allowed() const noexcept { return verdict_ == RiskVerdict::kAllow; }

  [[nodiscard]] RiskVerdict Verdict() const noexcept { return verdict_; }

  [[nodiscard]] Money OrderNotional() const noexcept { return order_notional_; }

  [[nodiscard]] std::span<const RejectReason> Reasons() const noexcept {
    return {
        reasons_.data(),
        reason_count_,
    };
  }

  [[nodiscard]] bool HasReason(RejectReason reason) const noexcept {
    const auto reasons = Reasons();

    return std::ranges::any_of(reasons, [reason](RejectReason current) { return current == reason; });
  }

  void AddReason(RejectReason reason) noexcept {
    if (reason_count_ < reasons_.size()) {
      auto* slot = reasons_.data() + reason_count_;
      *slot = reason;
      ++reason_count_;
    }

    verdict_ = RiskVerdict::kReject;
  }

  void SetOrderNotional(Money notional) noexcept { order_notional_ = notional; }

  void AllowIfNoRejections() noexcept { verdict_ = reason_count_ == 0 ? RiskVerdict::kAllow : RiskVerdict::kReject; }

 private:
  RiskVerdict verdict_{RiskVerdict::kReject};
  Money order_notional_{};

  std::array<RejectReason, kMaxRejectReasons> reasons_{};
  std::size_t reason_count_{};
};

}  // namespace botguard::risk
