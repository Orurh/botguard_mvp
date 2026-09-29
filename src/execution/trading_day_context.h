#pragma once

#include "risk/types.h"

#include <compare>
#include <cstdint>

namespace botguard::execution {

struct TradingDayContext {
  std::int64_t utc_day{};
  // First authoritative equity observed and durably fixed for this UTC-day
  // capability. It is not claimed to be equity exactly at 00:00:00 UTC.
  risk::Money baseline_equity{};

  auto operator<=>(const TradingDayContext&) const = default;
};

}  // namespace botguard::execution
