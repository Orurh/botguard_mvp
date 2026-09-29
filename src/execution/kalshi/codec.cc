#include "codec.h"

#include <openssl/sha.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <memory>

namespace botguard::execution::kalshi_internal {

std::optional<Json::Value> ParseJson(std::string_view input) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["failIfExtra"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errors;
  if (!reader->parse(input.data(), input.data() + input.size(), &root, &errors)) {
    return std::nullopt;
  }
  return root;
}

std::string JsonText(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

std::optional<std::int64_t> CheckedAdd(std::int64_t lhs, std::int64_t rhs) noexcept {
  if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
    return std::nullopt;
  }
  return lhs + rhs;
}

std::optional<std::int64_t> ParseFixedMicros(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  const auto dot = text.find('.');
  const auto whole_text = text.substr(0, dot);
  auto fractional = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (whole_text.empty() || fractional.size() > 6 || (dot != std::string_view::npos && fractional.empty())) {
    return std::nullopt;
  }
  std::int64_t whole{};
  const auto whole_result = std::from_chars(whole_text.data(), whole_text.data() + whole_text.size(), whole);
  if (whole_result.ec != std::errc{} || whole_result.ptr != whole_text.data() + whole_text.size() || whole < 0 ||
      whole > std::numeric_limits<std::int64_t>::max() / risk::kMicrosPerUsd) {
    return std::nullopt;
  }
  std::int64_t fraction{};
  if (!fractional.empty()) {
    const auto fraction_result = std::from_chars(fractional.data(), fractional.data() + fractional.size(), fraction);
    if (fraction_result.ec != std::errc{} || fraction_result.ptr != fractional.data() + fractional.size()) {
      return std::nullopt;
    }
    for (std::size_t digits = fractional.size(); digits < 6; ++digits) {
      fraction *= 10;
    }
  }
  const auto magnitude = CheckedAdd(whole * risk::kMicrosPerUsd, fraction);
  if (!magnitude) {
    return std::nullopt;
  }
  return negative ? -*magnitude : *magnitude;
}

std::string FormatFixed(std::int64_t micros, std::int64_t quantum, int digits) {
  const auto scaled = micros / quantum;
  std::array<char, 64> buffer{};
  const auto denominator = static_cast<std::int64_t>(digits == 4 ? 10'000 : 100);
  const auto whole = scaled / denominator;
  const auto fraction = scaled % denominator;
  const auto count = std::snprintf(buffer.data(), buffer.size(), digits == 4 ? "%lld.%04lld" : "%lld.%02lld", static_cast<long long>(whole),
                                   static_cast<long long>(fraction));
  return count > 0 ? std::string(buffer.data(), static_cast<std::size_t>(count)) : std::string{};
}

std::string ClientOrderUuid(risk::ClientOrderId id) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  const auto input = std::to_string(id);
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data());
  digest[6] = static_cast<unsigned char>((digest[6] & 0x0FU) | 0x50U);
  digest[8] = static_cast<unsigned char>((digest[8] & 0x3FU) | 0x80U);
  std::array<char, 37> output{};
  std::snprintf(output.data(), output.size(), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", digest[0], digest[1],
                digest[2], digest[3], digest[4], digest[5], digest[6], digest[7], digest[8], digest[9], digest[10], digest[11], digest[12],
                digest[13], digest[14], digest[15]);
  return output.data();
}

std::optional<VenueOrderState> DecodeOrderState(std::string_view status) noexcept {
  if (status == "resting") {
    return VenueOrderState::kOpen;
  }
  if (status == "executed") {
    return VenueOrderState::kFilled;
  }
  if (status == "canceled") {
    return VenueOrderState::kCancelled;
  }
  if (status == "rejected") {
    return VenueOrderState::kRejected;
  }
  return std::nullopt;
}

bool IsSafeTicker(std::string_view ticker) noexcept {
  return !ticker.empty() &&
         ticker.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.") == std::string_view::npos;
}

bool ValidateOrderbookSide(const Json::Value& levels) noexcept {
  if (!levels.isArray()) {
    return false;
  }
  for (const auto& level : levels) {
    if (!level.isArray() || level.size() != 2 || !level[0].isString() || !level[1].isString()) {
      return false;
    }
    const auto price = ParseFixedMicros(level[0].asString());
    const auto quantity = ParseFixedMicros(level[1].asString());
    if (!price || !quantity || *price <= 0 || *price >= risk::kMicrosPerUsd || *quantity <= 0) {
      return false;
    }
  }
  return true;
}

}  // namespace botguard::execution::kalshi_internal
