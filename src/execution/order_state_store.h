#pragma once

#include "risk/order_registry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace botguard::execution {

// Recovery persistence boundary for order lifecycle state.
//
// Current threading contract:
// - single writer;
// - all calls are serialized by the owning execution thread.
//
enum class OrderAuditEventType : std::uint8_t {
  kIntentPendingDurable = 1,
  kAbortedBeforeSubmit = 2,
  kVenueOpen = 3,
  kVenueFilled = 4,
  kVenueRejected = 5,
  kVenueUnknown = 6,
  kReconciledOpen = 7,
  kReconciledFilled = 8,
  kReconciledCancelled = 9,
  kReconciledRejected = 10,
  kExposureReconciled = 11,
};

struct OrderAuditEvent {
  std::uint64_t sequence{};
  risk::ClientOrderId client_order_id{};
  OrderAuditEventType type{OrderAuditEventType::kIntentPendingDurable};
  risk::OrderState resulting_state{risk::OrderState::kPendingSubmit};
  std::optional<std::string> venue_order_id;

  auto operator<=>(const OrderAuditEvent&) const = default;
};

[[nodiscard]] const char* ToString(OrderAuditEventType type) noexcept;

// PersistTransition contract:
// - true means entry and its matching audit event are committed atomically;
// - false means the previously committed value, if any, remains intact;
// - retrying the same semantic event with the same immutable order identity
//   and reserved notional is idempotent and must not append a duplicate or
//   overwrite a later state;
// - an existing event with different immutable identity or reserved notional
//   must fail rather than claim that the supplied entry is durable;
// - replacement of one client_order_id is atomic.
//
// LoadAll contract:
// - on true, entries contains at most one latest committed entry
//   per client_order_id;
// - on false, recovery state must be treated as untrusted and startup
//   must remain fail closed.
//
// A production implementation must not report true until its required
// durability barrier has completed.
class OrderStateStore {
 public:
  virtual ~OrderStateStore() = default;

  [[nodiscard]] virtual bool PersistTransition(const risk::OrderRegistryEntry& entry, OrderAuditEventType event_type) noexcept = 0;

  [[nodiscard]] virtual bool LoadAll(std::vector<risk::OrderRegistryEntry>& entries) noexcept = 0;

  [[nodiscard]] virtual bool LoadAuditEvents(std::vector<OrderAuditEvent>& events) noexcept = 0;
};

[[nodiscard]] risk::OrderRegistryEntry MakeOrderStateEntry(risk::ClientOrderId client_order_id, const risk::OrderRecord& record);

[[nodiscard]] bool PersistCurrentOrderState(OrderStateStore& state_store, const risk::OrderRegistry& registry,
                                            risk::ClientOrderId client_order_id, OrderAuditEventType event_type) noexcept;

}  // namespace botguard::execution
