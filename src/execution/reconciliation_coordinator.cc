#include "reconciliation_coordinator.h"

namespace botguard::execution {
namespace {

[[nodiscard]] std::optional<OrderAuditEventType> AuditTypeForReconciliation(risk::OrderState state) noexcept {
  switch (state) {
    case risk::OrderState::kOpen:
      return OrderAuditEventType::kReconciledOpen;
    case risk::OrderState::kFilled:
      return OrderAuditEventType::kReconciledFilled;
    case risk::OrderState::kCancelled:
      return OrderAuditEventType::kReconciledCancelled;
    case risk::OrderState::kRejected:
      return OrderAuditEventType::kReconciledRejected;
    case risk::OrderState::kPendingSubmit:
    case risk::OrderState::kAbortedBeforeSubmit:
    case risk::OrderState::kUnknown:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

ReconciliationCoordinator::ReconciliationCoordinator(risk::OrderRegistry& registry, OrderStateStore& state_store) noexcept
    : registry_(registry), state_store_(state_store) {}

ReconciliationUpdateStatus ReconciliationCoordinator::MarkOpen(risk::ClientOrderId client_order_id) noexcept {
  return ApplyAndPersist(client_order_id, &risk::OrderRegistry::MarkOpen, OrderAuditEventType::kReconciledOpen);
}

ReconciliationUpdateStatus ReconciliationCoordinator::MarkFilled(risk::ClientOrderId client_order_id) noexcept {
  return ApplyAndPersist(client_order_id, &risk::OrderRegistry::MarkFilled, OrderAuditEventType::kReconciledFilled);
}

ReconciliationUpdateStatus ReconciliationCoordinator::MarkCancelled(risk::ClientOrderId client_order_id) noexcept {
  return ApplyAndPersist(client_order_id, &risk::OrderRegistry::MarkCancelled, OrderAuditEventType::kReconciledCancelled);
}

ReconciliationUpdateStatus ReconciliationCoordinator::MarkRejected(risk::ClientOrderId client_order_id) noexcept {
  return ApplyAndPersist(client_order_id, &risk::OrderRegistry::MarkRejected, OrderAuditEventType::kReconciledRejected);
}

ReconciliationUpdateStatus ReconciliationCoordinator::ApplyVenueObservation(risk::ClientOrderId client_order_id, risk::OrderState state,
                                                                            std::optional<std::string> venue_order_id) noexcept {
  const auto event_type = AuditTypeForReconciliation(state);
  if (!event_type) {
    healthy_ = false;
    return ReconciliationUpdateStatus::kRegistryMutationFailed;
  }
  if (!registry_.get().ApplyVenueUpdate(client_order_id, state, std::move(venue_order_id))) {
    healthy_ = false;
    return ReconciliationUpdateStatus::kRegistryMutationFailed;
  }

  if (!PersistCurrent(client_order_id, *event_type)) {
    healthy_ = false;
    return ReconciliationUpdateStatus::kPersistenceFailed;
  }

  return ReconciliationUpdateStatus::kApplied;
}

ReconciliationUpdateStatus ReconciliationCoordinator::MarkExposureReconciled(risk::ClientOrderId client_order_id) noexcept {
  return ApplyAndPersist(client_order_id, &risk::OrderRegistry::MarkExposureReconciled, OrderAuditEventType::kExposureReconciled);
}

std::optional<risk::OrderRecord> ReconciliationCoordinator::Record(risk::ClientOrderId client_order_id) const {
  return registry_.get().Record(client_order_id);
}

std::vector<risk::OrderRegistryEntry> ReconciliationCoordinator::Snapshot() const {
  return registry_.get().Snapshot();
}

bool ReconciliationCoordinator::Healthy() const noexcept {
  return healthy_;
}

ReconciliationUpdateStatus ReconciliationCoordinator::ApplyAndPersist(risk::ClientOrderId client_order_id, RegistryMutation mutation,
                                                                      OrderAuditEventType event_type) noexcept {
  auto& registry = registry_.get();

  if (!(registry.*mutation)(client_order_id)) {
    healthy_ = false;
    return ReconciliationUpdateStatus::kRegistryMutationFailed;
  }

  if (!PersistCurrent(client_order_id, event_type)) {
    healthy_ = false;
    // Runtime state may now be newer than durable state.
    //
    // The caller must treat this reconciliation attempt as failed.
    // During startup no StartupRecoverySession will be produced.
    return ReconciliationUpdateStatus::kPersistenceFailed;
  }

  return ReconciliationUpdateStatus::kApplied;
}

bool ReconciliationCoordinator::PersistCurrent(risk::ClientOrderId client_order_id, OrderAuditEventType event_type) noexcept {
  return PersistCurrentOrderState(state_store_, registry_, client_order_id, event_type);
}

}  // namespace botguard::execution
