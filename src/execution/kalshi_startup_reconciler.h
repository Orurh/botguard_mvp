#pragma once

#include "startup_recovery.h"
#include "trading_day_context.h"

#include <functional>

namespace botguard::execution {

class KalshiAdapter;

// Kalshi-specific startup orchestration. Venue observations are applied and
// persisted before the authoritative account snapshot is refreshed, ensuring
// that a successful recovery baseline is causally newer than order recovery.
class KalshiStartupReconciler final : public StartupReconciler {
 public:
  KalshiStartupReconciler(KalshiAdapter& adapter, TradingDayContext context) noexcept : adapter_(adapter), context_(context) {}

  [[nodiscard]] bool Reconcile(ReconciliationCoordinator& reconciliation) noexcept override;

 private:
  std::reference_wrapper<KalshiAdapter> adapter_;
  TradingDayContext context_;
};

}  // namespace botguard::execution
