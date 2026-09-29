#include "order_registry.h"
#include "order_notional.h"

#include <limits>
#include <optional>
#include <utility>

namespace botguard::risk {

const char* ToString(OrderState state) noexcept {
  switch (state) {
    case OrderState::kPendingSubmit:
      return "PENDING_SUBMIT";
    case OrderState::kOpen:
      return "OPEN";
    case OrderState::kFilled:
      return "FILLED";
    case OrderState::kCancelled:
      return "CANCELLED";
    case OrderState::kRejected:
      return "REJECTED";
    case OrderState::kAbortedBeforeSubmit:
      return "ABORTED_BEFORE_SUBMIT";
    case OrderState::kUnknown:
      return "UNKNOWN";
  }
  return "INVALID";
}
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

[[nodiscard]] constexpr bool IsValidSide(Side side) noexcept {
  switch (side) {
    case Side::kBuy:
    case Side::kSell:
      return true;
  }

  return false;
}

[[nodiscard]] constexpr bool IsRestoredEntryValid(const OrderRegistryEntry& entry) noexcept {
  if (entry.reserved_notional.micros <= 0 || entry.identity.client_order_id != entry.client_order_id ||
      entry.identity.market_id != entry.market_id || !IsValidSide(entry.identity.side) || entry.identity.price.micros_per_unit <= 0 ||
      entry.identity.quantity.microunits <= 0 ||
      (entry.venue_order_id && (entry.venue_order_id->empty() || entry.venue_order_id->size() > kMaxVenueOrderIdBytes))) {
    return false;
  }

  const auto required_notional = ComputeOrderNotional(entry.identity.price, entry.identity.quantity);
  if (!required_notional || entry.reserved_notional < *required_notional) {
    return false;
  }

  switch (entry.state) {
    case OrderState::kPendingSubmit:
    case OrderState::kOpen:
    case OrderState::kUnknown:
      // These states may still create economic exposure and therefore
      // must always retain their reservation.
      return entry.reservation_active;

    case OrderState::kFilled:
      // FILLED may still have a reservation while waiting for the
      // authoritative account state to reflect the fill.
      //
      // After MarkExposureReconciled() the same FILLED record remains,
      // but its reservation becomes inactive.
      return true;

    case OrderState::kCancelled:
    case OrderState::kRejected:
    case OrderState::kAbortedBeforeSubmit:
      // Definitive terminal states cannot retain risk capacity.
      return !entry.reservation_active;
  }

  // Handles corrupted/out-of-range enum values.
  return false;
}

}  // namespace

bool OrderRegistry::BeginIntent(const OrderIntent& intent, Money reserved_notional) {
  if (reserved_notional.micros <= 0) {
    return false;
  }

  if (states_.contains(intent.client_order_id)) {
    return false;
  }

  const auto new_total = CheckedAddNonNegative(total_reserved_exposure_.micros, reserved_notional.micros);

  if (!new_total) {
    return false;
  }

  const auto current_market = MarketReservedExposure(intent.market_id);

  const auto new_market = CheckedAddNonNegative(current_market.micros, reserved_notional.micros);

  if (!new_market) {
    return false;
  }

  // Ensure the market bucket exists before claiming the intent.
  // A failed allocation here therefore cannot leave a claimed order
  // without its aggregate reservation bucket.
  const auto [market_iterator, market_inserted] = market_reserved_exposure_.try_emplace(intent.market_id, current_market);

  static_cast<void>(market_inserted);

  const auto [state_iterator, inserted] = states_.try_emplace(intent.client_order_id, OrderRecord{
                                                                                          .state = OrderState::kPendingSubmit,
                                                                                          .market_id = intent.market_id,
                                                                                          .reserved_notional = reserved_notional,
                                                                                          .reservation_active = true,
                                                                                          .identity =
                                                                                              OrderIdentity{
                                                                                                  .strategy_id = intent.strategy_id,
                                                                                                  .client_order_id = intent.client_order_id,
                                                                                                  .market_id = intent.market_id,
                                                                                                  .side = intent.side,
                                                                                                  .price = intent.price,
                                                                                                  .quantity = intent.quantity,
                                                                                              },
                                                                                          .venue_order_id = std::nullopt,
                                                                                      });

  static_cast<void>(state_iterator);

  if (!inserted) {
    return false;
  }

  market_iterator->second.micros = *new_market;
  total_reserved_exposure_.micros = *new_total;

  return true;
}

bool OrderRegistry::RollbackUnpersistedIntent(ClientOrderId client_order_id) {
  const auto iterator = states_.find(client_order_id);

  if (iterator == states_.end()) {
    return false;
  }

  auto& record = iterator->second;

  if (record.state != OrderState::kPendingSubmit || !record.reservation_active) {
    return false;
  }

  const auto market_id = record.market_id;

  ReleaseReservation(record);
  states_.erase(iterator);

  const auto market_iterator = market_reserved_exposure_.find(market_id);

  if (market_iterator != market_reserved_exposure_.end() && market_iterator->second.micros == 0) {
    market_reserved_exposure_.erase(market_iterator);
  }

  return true;
}

bool OrderRegistry::MarkOpen(ClientOrderId client_order_id) {
  return Transition(client_order_id, OrderState::kOpen);
}

bool OrderRegistry::MarkFilled(ClientOrderId client_order_id) {
  return Transition(client_order_id, OrderState::kFilled);
}

bool OrderRegistry::MarkCancelled(ClientOrderId client_order_id) {
  return Transition(client_order_id, OrderState::kCancelled);
}

bool OrderRegistry::MarkRejected(ClientOrderId client_order_id) {
  return Transition(client_order_id, OrderState::kRejected);
}

bool OrderRegistry::MarkAbortedBeforeSubmit(ClientOrderId client_order_id) {
  return Transition(client_order_id, OrderState::kAbortedBeforeSubmit);
}

bool OrderRegistry::MarkUnknown(ClientOrderId client_order_id) {
  return Transition(client_order_id, OrderState::kUnknown);
}

bool OrderRegistry::ApplyVenueUpdate(ClientOrderId client_order_id, OrderState next_state, std::optional<std::string> venue_order_id) {
  const auto iterator = states_.find(client_order_id);

  if (iterator == states_.end() || (venue_order_id && (venue_order_id->empty() || venue_order_id->size() > kMaxVenueOrderIdBytes))) {
    return false;
  }

  auto& record = iterator->second;

  if (record.state != next_state && !IsTransitionAllowed(record.state, next_state)) {
    return false;
  }

  if (record.venue_order_id && venue_order_id && *record.venue_order_id != *venue_order_id) {
    return false;
  }

  if (!record.venue_order_id && venue_order_id) {
    record.venue_order_id = std::move(venue_order_id);
  }

  record.state = next_state;

  if (next_state == OrderState::kCancelled || next_state == OrderState::kRejected || next_state == OrderState::kAbortedBeforeSubmit) {
    ReleaseReservation(record);
  }

  return true;
}

bool OrderRegistry::MarkExposureReconciled(ClientOrderId client_order_id) {
  const auto iterator = states_.find(client_order_id);

  if (iterator == states_.end()) {
    return false;
  }

  auto& record = iterator->second;

  if (record.state != OrderState::kFilled) {
    return false;
  }

  ReleaseReservation(record);
  return true;
}

bool OrderRegistry::Contains(ClientOrderId client_order_id) const {
  return states_.contains(client_order_id);
}

std::optional<OrderState> OrderRegistry::State(ClientOrderId client_order_id) const {
  const auto record = Record(client_order_id);

  if (!record) {
    return std::nullopt;
  }

  return record->state;
}

std::optional<OrderRecord> OrderRegistry::Record(ClientOrderId client_order_id) const {
  const auto iterator = states_.find(client_order_id);

  if (iterator == states_.end()) {
    return std::nullopt;
  }

  return iterator->second;
}

std::optional<Money> OrderRegistry::ReservedNotional(ClientOrderId client_order_id) const noexcept {
  const auto iterator = states_.find(client_order_id);

  if (iterator == states_.end()) {
    return std::nullopt;
  }

  return iterator->second.reserved_notional;
}

Money OrderRegistry::TotalReservedExposure() const noexcept {
  return total_reserved_exposure_;
}

Money OrderRegistry::MarketReservedExposure(MarketId market_id) const {
  const auto iterator = market_reserved_exposure_.find(market_id);

  if (iterator == market_reserved_exposure_.end()) {
    return Money{};
  }

  return iterator->second;
}

std::vector<OrderRegistryEntry> OrderRegistry::Snapshot() const {
  std::vector<OrderRegistryEntry> entries;
  entries.reserve(states_.size());

  for (const auto& [client_order_id, record] : states_) {
    entries.push_back(OrderRegistryEntry{
        .client_order_id = client_order_id,
        .state = record.state,
        .market_id = record.market_id,
        .reserved_notional = record.reserved_notional,
        .reservation_active = record.reservation_active,
        .identity = record.identity,
        .venue_order_id = record.venue_order_id,
    });
  }

  return entries;
}

bool OrderRegistry::Restore(std::span<const OrderRegistryEntry> entries) {
  OrderRegistry restored;

  restored.states_.reserve(entries.size());
  restored.market_reserved_exposure_.reserve(entries.size());

  for (const auto& entry : entries) {
    if (!IsRestoredEntryValid(entry)) {
      return false;
    }

    const auto [state_iterator, inserted] =
        restored.states_.try_emplace(entry.client_order_id, OrderRecord{
                                                                .state = entry.state,
                                                                .market_id = entry.market_id,
                                                                .reserved_notional = entry.reserved_notional,
                                                                .reservation_active = entry.reservation_active,
                                                                .identity = entry.identity,
                                                                .venue_order_id = entry.venue_order_id,
                                                            });

    static_cast<void>(state_iterator);

    // Duplicate client_order_id means corrupted/inconsistent
    // persisted identity state.
    if (!inserted) {
      return false;
    }

    if (!entry.reservation_active) {
      continue;
    }

    const auto new_total = CheckedAddNonNegative(restored.total_reserved_exposure_.micros, entry.reserved_notional.micros);

    if (!new_total) {
      return false;
    }

    const auto market_iterator = restored.market_reserved_exposure_.find(entry.market_id);

    const auto current_market = market_iterator == restored.market_reserved_exposure_.end() ? Money{} : market_iterator->second;

    const auto new_market = CheckedAddNonNegative(current_market.micros, entry.reserved_notional.micros);

    if (!new_market) {
      return false;
    }

    if (market_iterator == restored.market_reserved_exposure_.end()) {
      restored.market_reserved_exposure_.emplace(entry.market_id, Money{
                                                                      .micros = *new_market,
                                                                  });
    } else {
      market_iterator->second.micros = *new_market;
    }

    restored.total_reserved_exposure_.micros = *new_total;
  }

  // Commit only after the entire snapshot has been validated.
  states_.swap(restored.states_);

  market_reserved_exposure_.swap(restored.market_reserved_exposure_);

  std::swap(total_reserved_exposure_, restored.total_reserved_exposure_);

  return true;
}

bool OrderRegistry::Transition(ClientOrderId client_order_id, OrderState next_state) {
  const auto iterator = states_.find(client_order_id);

  if (iterator == states_.end()) {
    return false;
  }

  auto& record = iterator->second;

  if (record.state == next_state) {
    return true;
  }

  if (!IsTransitionAllowed(record.state, next_state)) {
    return false;
  }

  record.state = next_state;

  if (next_state == OrderState::kCancelled || next_state == OrderState::kRejected || next_state == OrderState::kAbortedBeforeSubmit) {
    ReleaseReservation(record);
  }

  return true;
}

void OrderRegistry::ReleaseReservation(OrderRecord& record) {
  if (!record.reservation_active) {
    return;
  }

  total_reserved_exposure_.micros -= record.reserved_notional.micros;

  const auto market_iterator = market_reserved_exposure_.find(record.market_id);

  if (market_iterator != market_reserved_exposure_.end()) {
    market_iterator->second.micros -= record.reserved_notional.micros;
  }

  record.reservation_active = false;
}

constexpr bool OrderRegistry::IsTransitionAllowed(OrderState current_state, OrderState next_state) noexcept {
  switch (current_state) {
    case OrderState::kPendingSubmit:
      return next_state == OrderState::kOpen || next_state == OrderState::kFilled || next_state == OrderState::kCancelled ||
             next_state == OrderState::kRejected || next_state == OrderState::kAbortedBeforeSubmit || next_state == OrderState::kUnknown;

    case OrderState::kOpen:
      return next_state == OrderState::kFilled || next_state == OrderState::kCancelled || next_state == OrderState::kUnknown;

    case OrderState::kUnknown:
      return next_state == OrderState::kOpen || next_state == OrderState::kFilled || next_state == OrderState::kCancelled ||
             next_state == OrderState::kRejected;

    case OrderState::kFilled:
    case OrderState::kCancelled:
    case OrderState::kRejected:
    case OrderState::kAbortedBeforeSubmit:
      return false;
  }

  return false;
}

}  // namespace botguard::risk
