#include "order_state_store.h"

namespace botguard::execution {

const char* ToString(OrderAuditEventType type) noexcept {
  switch (type) {
    case OrderAuditEventType::kIntentPendingDurable:
      return "INTENT_PENDING_DURABLE";
    case OrderAuditEventType::kAbortedBeforeSubmit:
      return "ABORTED_BEFORE_SUBMIT";
    case OrderAuditEventType::kVenueOpen:
      return "VENUE_OPEN";
    case OrderAuditEventType::kVenueFilled:
      return "VENUE_FILLED";
    case OrderAuditEventType::kVenueRejected:
      return "VENUE_REJECTED";
    case OrderAuditEventType::kVenueUnknown:
      return "VENUE_UNKNOWN";
    case OrderAuditEventType::kReconciledOpen:
      return "RECONCILED_OPEN";
    case OrderAuditEventType::kReconciledFilled:
      return "RECONCILED_FILLED";
    case OrderAuditEventType::kReconciledCancelled:
      return "RECONCILED_CANCELLED";
    case OrderAuditEventType::kReconciledRejected:
      return "RECONCILED_REJECTED";
    case OrderAuditEventType::kExposureReconciled:
      return "EXPOSURE_RECONCILED";
  }
  return "INVALID";
}

risk::OrderRegistryEntry MakeOrderStateEntry(risk::ClientOrderId client_order_id, const risk::OrderRecord& record) {
  return risk::OrderRegistryEntry{
      .client_order_id = client_order_id,
      .state = record.state,
      .market_id = record.market_id,
      .reserved_notional = record.reserved_notional,
      .reservation_active = record.reservation_active,
      .identity = record.identity,
      .venue_order_id = record.venue_order_id,
  };
}

bool PersistCurrentOrderState(OrderStateStore& state_store, const risk::OrderRegistry& registry, risk::ClientOrderId client_order_id,
                              OrderAuditEventType event_type) noexcept {
  const auto record = registry.Record(client_order_id);
  return record && state_store.PersistTransition(MakeOrderStateEntry(client_order_id, *record), event_type);
}

}  // namespace botguard::execution
