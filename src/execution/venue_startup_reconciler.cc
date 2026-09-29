#include "venue_startup_reconciler.h"

#include <utility>

namespace botguard::execution {
namespace {

[[nodiscard]] risk::OrderState ToOrderState(VenueOrderState state) noexcept {
  switch (state) {
    case VenueOrderState::kOpen:
      return risk::OrderState::kOpen;
    case VenueOrderState::kFilled:
      return risk::OrderState::kFilled;
    case VenueOrderState::kCancelled:
      return risk::OrderState::kCancelled;
    case VenueOrderState::kRejected:
      return risk::OrderState::kRejected;
  }

  return risk::OrderState::kUnknown;
}

}  // namespace

bool VenueStartupReconciler::Reconcile(ReconciliationCoordinator& reconciliation) noexcept {
  for (const auto& entry : reconciliation.Snapshot()) {
    if (entry.state != risk::OrderState::kPendingSubmit && entry.state != risk::OrderState::kUnknown) {
      continue;
    }

    auto result = query_.get().Query(VenueOrderLookupKey{
        .identity = entry.identity,
        .venue_order_id = entry.venue_order_id,
    });

    if (!result) {
      return false;
    }

    // Definitive absence proves that the ambiguous submission did not create
    // venue exposure. Persisting this transition records RECONCILED_REJECTED.
    const auto update = result->has_value() ? reconciliation.ApplyVenueObservation(entry.client_order_id, ToOrderState((*result)->state),
                                                                                   std::move((*result)->venue_order_id))
                                            : reconciliation.MarkRejected(entry.client_order_id);

    if (update != ReconciliationUpdateStatus::kApplied) {
      return false;
    }
  }

  return true;
}

}  // namespace botguard::execution
