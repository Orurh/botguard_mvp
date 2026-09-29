#pragma once

#include "startup_recovery.h"
#include "venue_order_query.h"

#include <functional>

namespace botguard::execution {

// Resolves restored PENDING_SUBMIT and UNKNOWN orders using an authoritative
// venue/account query. Any unavailable or invalid query keeps startup closed.
class VenueStartupReconciler final : public StartupReconciler {
 public:
  explicit VenueStartupReconciler(VenueOrderQuery& query) noexcept : query_(query) {}

  [[nodiscard]] bool Reconcile(ReconciliationCoordinator& reconciliation) noexcept override;

 private:
  std::reference_wrapper<VenueOrderQuery> query_;
};

}  // namespace botguard::execution
