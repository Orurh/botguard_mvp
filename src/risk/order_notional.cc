#include "order_notional.h"

#include <cstdint>
#include <limits>

namespace botguard::risk {
namespace {

[[nodiscard]] constexpr std::optional<std::int64_t> CheckedAddNonNegative(std::int64_t lhs, std::int64_t rhs) noexcept {
  if (lhs < 0 || rhs < 0 || lhs > std::numeric_limits<std::int64_t>::max() - rhs) {
    return std::nullopt;
  }
  return lhs + rhs;
}

[[nodiscard]] constexpr std::optional<std::int64_t> CheckedMulNonNegative(std::int64_t lhs, std::int64_t rhs) noexcept {
  if (lhs < 0 || rhs < 0) {
    return std::nullopt;
  }
  std::int64_t result{};
  if (__builtin_mul_overflow(lhs, rhs, &result)) {
    return std::nullopt;
  }
  return result;
}

}  // namespace

std::optional<Money> ComputeOrderNotional(Price price, Quantity quantity) noexcept {
  if (price.micros_per_unit <= 0 || quantity.microunits <= 0) {
    return std::nullopt;
  }

  const auto whole_units = quantity.microunits / kMicrounitsPerUnit;
  const auto fractional_units = quantity.microunits % kMicrounitsPerUnit;
  const auto whole_notional = CheckedMulNonNegative(price.micros_per_unit, whole_units);
  if (!whole_notional) {
    return std::nullopt;
  }
  if (fractional_units == 0) {
    return Money{.micros = *whole_notional};
  }

  const auto price_whole = price.micros_per_unit / kMicrounitsPerUnit;
  const auto price_fraction = price.micros_per_unit % kMicrounitsPerUnit;
  const auto fractional_base = price_whole * fractional_units;
  const auto remainder_product = price_fraction * fractional_units;
  const auto quotient = remainder_product / kMicrounitsPerUnit;
  const auto fractional_tail = quotient + static_cast<std::int64_t>(remainder_product % kMicrounitsPerUnit != 0);
  const auto fractional_notional = CheckedAddNonNegative(fractional_base, fractional_tail);
  if (!fractional_notional) {
    return std::nullopt;
  }
  const auto total = CheckedAddNonNegative(*whole_notional, *fractional_notional);
  return total ? std::optional<Money>{Money{.micros = *total}} : std::nullopt;
}

}  // namespace botguard::risk
