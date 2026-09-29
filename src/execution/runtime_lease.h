#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>

namespace botguard::execution {

enum class RuntimeLeaseError : std::uint8_t {
  kOpenFailed,
  kUnsafeLockFile,
  kAlreadyHeld,
  kUnsupportedPlatform,
};

// Process-wide exclusive ownership token for one order-state database.
// The adjacent .lock file is intentionally persistent; exclusivity is held by
// the OS lock on its descriptor and is released on destruction/process exit.
class RuntimeLease final {
 public:
  [[nodiscard]] static std::expected<RuntimeLease, RuntimeLeaseError> Acquire(const std::filesystem::path& order_database_path) noexcept;

  ~RuntimeLease();

  RuntimeLease(const RuntimeLease&) = delete;
  RuntimeLease& operator=(const RuntimeLease&) = delete;
  RuntimeLease(RuntimeLease&& other) noexcept;
  RuntimeLease& operator=(RuntimeLease&& other) noexcept;

 private:
  explicit RuntimeLease(int descriptor) noexcept : descriptor_(descriptor) {}
  void Close() noexcept;

  int descriptor_{-1};
};

}  // namespace botguard::execution
