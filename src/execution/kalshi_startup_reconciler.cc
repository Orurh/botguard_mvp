#include "kalshi_startup_reconciler.h"

#include "kalshi_adapter.h"
#include "venue_startup_reconciler.h"

namespace botguard::execution {

bool KalshiStartupReconciler::Reconcile(ReconciliationCoordinator& reconciliation) noexcept {
  VenueStartupReconciler venue_reconciler(adapter_.get());
  if (!venue_reconciler.Reconcile(reconciliation)) {
    return false;
  }
  return adapter_.get().RefreshAccountState(context_);
}

}  // namespace botguard::execution
