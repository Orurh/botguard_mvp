#pragma once

#include "order_state_store.h"

#include "risk/order_registry.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace botguard::execution {

enum class ReconciliationUpdateStatus : std::uint8_t {
  kApplied,
  kRegistryMutationFailed,
  kPersistenceFailed,
};

// Applies authoritative reconciliation results to the local lifecycle
// registry and durably persists every successful mutation.
//
// Not thread-safe.
// Must be called from the owning execution/recovery thread.
class ReconciliationCoordinator {
 public:
  ReconciliationCoordinator(risk::OrderRegistry& registry, OrderStateStore& state_store) noexcept;

  [[nodiscard]] ReconciliationUpdateStatus MarkOpen(risk::ClientOrderId client_order_id) noexcept;

  [[nodiscard]] ReconciliationUpdateStatus MarkFilled(risk::ClientOrderId client_order_id) noexcept;

  [[nodiscard]] ReconciliationUpdateStatus MarkCancelled(risk::ClientOrderId client_order_id) noexcept;

  [[nodiscard]] ReconciliationUpdateStatus MarkRejected(risk::ClientOrderId client_order_id) noexcept;

  [[nodiscard]] ReconciliationUpdateStatus ApplyVenueObservation(risk::ClientOrderId client_order_id, risk::OrderState state,
                                                                 std::optional<std::string> venue_order_id) noexcept;

  // Releases the local FILLED reservation only after authoritative
  // account state is known to include the resulting exposure.
  [[nodiscard]] ReconciliationUpdateStatus MarkExposureReconciled(risk::ClientOrderId client_order_id) noexcept;

  [[nodiscard]] std::optional<risk::OrderRecord> Record(risk::ClientOrderId client_order_id) const;

  [[nodiscard]] std::vector<risk::OrderRegistryEntry> Snapshot() const;

  // False after any requested registry mutation or durable write
  // failed. Startup recovery uses this independently of the adapter's
  // return value, so a buggy reconciler cannot accidentally produce a
  // trading capability after ignoring an update failure.
  [[nodiscard]] bool Healthy() const noexcept;

 private:
  using RegistryMutation = bool (risk::OrderRegistry::*)(risk::ClientOrderId);

  [[nodiscard]] ReconciliationUpdateStatus ApplyAndPersist(risk::ClientOrderId client_order_id, RegistryMutation mutation,
                                                           OrderAuditEventType event_type) noexcept;

  [[nodiscard]] bool PersistCurrent(risk::ClientOrderId client_order_id, OrderAuditEventType event_type) noexcept;

  std::reference_wrapper<risk::OrderRegistry> registry_;
  std::reference_wrapper<OrderStateStore> state_store_;
  bool healthy_{true};
};

}  // namespace botguard::execution
