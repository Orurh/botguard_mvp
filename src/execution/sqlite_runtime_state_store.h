#pragma once

#include "risk/types.h"
#include "storage_generation.h"
#include "trading_day_context.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

struct sqlite3;

namespace botguard::execution {

struct RuntimeBinding {
  // Lowercase SHA-256 hex digest of the immutable venue/account identity.
  std::string fingerprint;
};

enum class SqliteRuntimeStateStoreCreateError : std::uint8_t {
  kOpenFailed,
  kConfigurationFailed,
  kSchemaInitializationFailed,
  kInvalidBinding,
  kBindingMismatch,
  kInvalidStorageGeneration,
  kStorageGenerationMismatch,
};

enum class DailyEquityBaselineError : std::uint8_t {
  kInvalidInput,
  kDayRegression,
  kPersistenceFailed,
  kCorruptState,
};

enum class RuntimeControlError : std::uint8_t {
  kPersistenceFailed,
  kCorruptState,
};

// Durable runtime safety state, intentionally separate from order lifecycle
// persistence. One file is permanently bound to one venue/account runtime.
// An indeterminate runtime-control COMMIT terminates the process instead of
// returning a result that could misstate the durable admission hold.
class SqliteRuntimeStateStore final {
 public:
  [[nodiscard]] static std::expected<SqliteRuntimeStateStore, SqliteRuntimeStateStoreCreateError> CreateNew(
      const std::filesystem::path& path, const RuntimeBinding& binding, const StorageGeneration& generation);
  [[nodiscard]] static std::expected<SqliteRuntimeStateStore, SqliteRuntimeStateStoreCreateError> OpenExisting(
      const std::filesystem::path& path, const RuntimeBinding& binding, const StorageGeneration& expected_generation);

  ~SqliteRuntimeStateStore();

  SqliteRuntimeStateStore(const SqliteRuntimeStateStore&) = delete;
  SqliteRuntimeStateStore& operator=(const SqliteRuntimeStateStore&) = delete;
  SqliteRuntimeStateStore(SqliteRuntimeStateStore&& other) noexcept;
  SqliteRuntimeStateStore& operator=(SqliteRuntimeStateStore&& other) noexcept;

  // First successful observation for a UTC day durably fixes
  // proposed_baseline_equity. This is a loss-since-baseline anchor, not proof
  // of equity exactly at the UTC-day boundary. Repeated calls for that day
  // return the stored value and cannot replace it. A newer day atomically
  // rotates the baseline; an older day fails closed.
  [[nodiscard]] std::expected<risk::Money, DailyEquityBaselineError> ResolveDailyBaselineEquity(
      std::int64_t trading_day_utc, risk::Money proposed_baseline_equity) noexcept;

  // Reads the durable day and baseline without rotating them. An absent
  // context is valid only before the first successful initialization.
  [[nodiscard]] std::expected<std::optional<TradingDayContext>, DailyEquityBaselineError> LoadTradingDayContext() noexcept;

  // Durable operator intent only. READY and RECOVERY_REQUIRED are derived
  // runtime states and are deliberately never persisted.
  [[nodiscard]] std::expected<bool, RuntimeControlError> OperatorKilled() noexcept;
  [[nodiscard]] std::expected<void, RuntimeControlError> SetOperatorKilled(bool killed) noexcept;

  [[nodiscard]] const StorageGeneration& Generation() const noexcept { return generation_; }

 private:
  [[nodiscard]] static std::expected<SqliteRuntimeStateStore, SqliteRuntimeStateStoreCreateError> OpenWithMode(
      const std::filesystem::path& path, const RuntimeBinding& binding, const StorageGeneration& generation, bool create_new);

  SqliteRuntimeStateStore(sqlite3* database, StorageGeneration generation) noexcept
      : database_(database), generation_(std::move(generation)) {}
  void Close() noexcept;

  sqlite3* database_{};
  StorageGeneration generation_;
};

}  // namespace botguard::execution
