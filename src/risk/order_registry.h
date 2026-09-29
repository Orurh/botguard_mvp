#pragma once

#include "types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace botguard::risk {

inline constexpr std::size_t kMaxVenueOrderIdBytes = 512;

enum class OrderState : std::uint8_t {
  kPendingSubmit,
  kOpen,
  kFilled,
  kCancelled,
  kRejected,
  kAbortedBeforeSubmit,
  kUnknown,
};

[[nodiscard]] const char* ToString(OrderState state) noexcept;

struct OrderIdentity {
  StrategyId strategy_id{};
  ClientOrderId client_order_id{};
  MarketId market_id{};
  Side side{Side::kBuy};
  Price price{};
  Quantity quantity{};

  auto operator<=>(const OrderIdentity&) const = default;
};

struct OrderRecord {
  OrderState state{OrderState::kPendingSubmit};
  MarketId market_id{};
  Money reserved_notional{};
  bool reservation_active{true};
  OrderIdentity identity{};
  std::optional<std::string> venue_order_id;
};

// Persistence-neutral representation of one registry entry.
// Aggregate exposure is intentionally not persisted and is rebuilt
// from these records during Restore().
struct OrderRegistryEntry {
  ClientOrderId client_order_id{};
  OrderState state{OrderState::kPendingSubmit};
  MarketId market_id{};
  Money reserved_notional{};
  bool reservation_active{};
  OrderIdentity identity{};
  std::optional<std::string> venue_order_id;
};

// Not thread-safe.
// All methods must be called from the owning risk-executor thread.
class OrderRegistry {
 public:
  // Claims client_order_id for one economic intent and reserves its
  // worst-case pending notional.
  //
  // Once an ID has been claimed durably, it can never be claimed again,
  // regardless of the current or terminal order state.
  [[nodiscard]] bool BeginIntent(const OrderIntent& intent, Money reserved_notional);

  // Rolls back only the in-memory half of BeginIntent() when the first
  // durable PENDING_SUBMIT transition failed and Submit() has not started.
  //
  // This is not a lifecycle transition: no durable identity or exchange
  // side effect exists, so the reservation and identity are removed.
  // The caller must never use this after successful durable transition persistence.
  [[nodiscard]] bool RollbackUnpersistedIntent(ClientOrderId client_order_id);

  [[nodiscard]] bool MarkOpen(ClientOrderId client_order_id);
  [[nodiscard]] bool MarkFilled(ClientOrderId client_order_id);
  [[nodiscard]] bool MarkCancelled(ClientOrderId client_order_id);
  [[nodiscard]] bool MarkRejected(ClientOrderId client_order_id);
  [[nodiscard]] bool MarkAbortedBeforeSubmit(ClientOrderId client_order_id);
  [[nodiscard]] bool MarkUnknown(ClientOrderId client_order_id);

  // Applies one authoritative venue observation as a single in-memory
  // mutation. State and acknowledgement are validated before either changes.
  [[nodiscard]] bool ApplyVenueUpdate(ClientOrderId client_order_id, OrderState next_state, std::optional<std::string> venue_order_id);

  // Releases a FILLED order reservation after authoritative account
  // state has been reconciled and already includes the resulting exposure.
  [[nodiscard]] bool MarkExposureReconciled(ClientOrderId client_order_id);

  [[nodiscard]] bool Contains(ClientOrderId client_order_id) const;

  [[nodiscard]] std::optional<OrderState> State(ClientOrderId client_order_id) const;

  [[nodiscard]] std::optional<OrderRecord> Record(ClientOrderId client_order_id) const;

  // Hot-path duplicate lookup. Returns only the fixed-size value needed by
  // OrderGate and never copies the optional venue acknowledgement string.
  [[nodiscard]] std::optional<Money> ReservedNotional(ClientOrderId client_order_id) const noexcept;

  [[nodiscard]] Money TotalReservedExposure() const noexcept;

  [[nodiscard]] Money MarketReservedExposure(MarketId market_id) const;

  // Not a hot-path operation. Intended for persistence/checkpointing.
  [[nodiscard]] std::vector<OrderRegistryEntry> Snapshot() const;

  // Replaces the entire registry state if every entry is valid.
  //
  // Restore is all-or-nothing: on validation failure the current
  // registry remains unchanged.
  //
  // Aggregate reservations are rebuilt from individual records.
  [[nodiscard]] bool Restore(std::span<const OrderRegistryEntry> entries);

 private:
  [[nodiscard]] bool Transition(ClientOrderId client_order_id, OrderState next_state);

  void ReleaseReservation(OrderRecord& record);

  [[nodiscard]] static constexpr bool IsTransitionAllowed(OrderState current_state, OrderState next_state) noexcept;

  std::unordered_map<ClientOrderId, OrderRecord> states_;
  std::unordered_map<MarketId, Money> market_reserved_exposure_;
  Money total_reserved_exposure_{};
};

}  // namespace botguard::risk
