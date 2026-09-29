#pragma once

#include "risk/types.h"

#include <optional>

namespace botguard::execution {

struct MarketDataCursor {
  risk::MarketId market_id{};
  std::uint64_t version{};
  risk::MonotonicClock::time_point received_at;
};

// Trusted execution-side evidence that BotGuard itself received venue market
// data. Strategy-supplied timestamps must never implement this boundary.
class MarketDataFreshnessProvider {
 public:
  virtual ~MarketDataFreshnessProvider() = default;

  [[nodiscard]] virtual std::optional<MarketDataCursor> Current(risk::MarketId market_id) const noexcept = 0;
};

}  // namespace botguard::execution
