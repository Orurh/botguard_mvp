#pragma once

#include "risk/types.h"

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>

namespace botguard::execution {

struct AccountStateVersion {
  std::uint64_t value{};

  auto operator<=>(const AccountStateVersion&) const = default;
};

// Account-wide watermark proving that the provider has received an
// authoritative account-state revision.
//
// The version must be monotonically non-decreasing for the lifetime
// of one venue/account binding.
struct AccountStateCursor {
  AccountStateVersion version{};
  risk::MonotonicClock::time_point received_at;
};

// Authoritative state used for risk evaluation of one market.
//
// market_gross_exposure corresponds to the requested market_id.
// total_gross_exposure and pnl_since_baseline correspond to the bound account.
struct AccountStateSnapshot {
  risk::AccountState state{};

  AccountStateVersion version{};

  risk::MonotonicClock::time_point received_at;
};

struct AccountStateSafetyPolicy {
  std::chrono::milliseconds max_age{};
};

// Read-only authoritative account-state boundary.
//
// V0 binding invariant:
// - one provider instance represents exactly one venue/account;
// - versions are monotonically non-decreasing;
// - Snapshot(market_id) returns exposure for that market from the
//   authoritative account-state stream;
// - caller-supplied strategy state must never enter this interface.
//
// Not thread-safe unless an implementation explicitly guarantees it.
// Current BotGuard V0 calls it from the owning execution thread.
class AuthoritativeAccountStateProvider {
 public:
  virtual ~AuthoritativeAccountStateProvider() = default;

  [[nodiscard]] virtual std::optional<AccountStateCursor> CurrentCursor() const noexcept = 0;

  [[nodiscard]] virtual std::optional<AccountStateSnapshot> Snapshot(risk::MarketId market_id) const noexcept = 0;
};

}  // namespace botguard::execution