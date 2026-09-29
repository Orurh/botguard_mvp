#pragma once

#include "execution/venue_order_query.h"

#include <json/json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace botguard::execution::kalshi_internal {

[[nodiscard]] std::optional<Json::Value> ParseJson(std::string_view input);
[[nodiscard]] std::string JsonText(const Json::Value& value);

[[nodiscard]] std::optional<std::int64_t> CheckedAdd(std::int64_t lhs, std::int64_t rhs) noexcept;
[[nodiscard]] std::optional<std::int64_t> ParseFixedMicros(std::string_view text) noexcept;
[[nodiscard]] std::string FormatFixed(std::int64_t micros, std::int64_t quantum, int digits);
[[nodiscard]] std::string ClientOrderUuid(risk::ClientOrderId id);

[[nodiscard]] std::optional<VenueOrderState> DecodeOrderState(std::string_view status) noexcept;
[[nodiscard]] bool IsSafeTicker(std::string_view ticker) noexcept;
[[nodiscard]] bool ValidateOrderbookSide(const Json::Value& levels) noexcept;

}  // namespace botguard::execution::kalshi_internal
