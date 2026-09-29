#pragma once

#include "types.h"

#include <optional>

namespace botguard::risk {

// Computes ceil(price * quantity) in Money micros without overflowing
// intermediate fixed-point products. Invalid or unrepresentable input fails.
[[nodiscard]] std::optional<Money> ComputeOrderNotional(Price price, Quantity quantity) noexcept;

}  // namespace botguard::risk
