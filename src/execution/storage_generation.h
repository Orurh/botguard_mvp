#pragma once

#include <string>
#include <string_view>

namespace botguard::execution {

struct StorageGeneration {
  // Lowercase hex-encoded 256-bit random identifier.
  std::string value;

  auto operator<=>(const StorageGeneration&) const = default;
};

[[nodiscard]] inline bool IsValidStorageGeneration(std::string_view value) noexcept {
  return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

}  // namespace botguard::execution
