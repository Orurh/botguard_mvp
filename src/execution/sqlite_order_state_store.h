#pragma once

#include "order_state_store.h"
#include "storage_generation.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>

struct sqlite3;
struct sqlite3_stmt;

namespace botguard::execution {

enum class SqliteOrderStateStoreError : std::uint8_t {
  kOpenFailed,
  kConfigurationFailed,
  kSchemaInitializationFailed,
  kInvalidStorageGeneration,
};

// Durable SQLite implementation of OrderStateStore.
//
// One instance owns one SQLite connection and is intentionally not
// thread-safe. All calls must stay on the owning execution thread.
// ClientOrderId and MarketId values above INT64_MAX are rejected because
// SQLite INTEGER is a signed 64-bit storage class.
// An indeterminate COMMIT outcome terminates the process instead of
// returning false and allowing an unsafe in-memory identity rollback.
class SqliteOrderStateStore final : public OrderStateStore {
 public:
  [[nodiscard]] static std::expected<SqliteOrderStateStore, SqliteOrderStateStoreError> CreateNew(const std::filesystem::path& path,
                                                                                                  const StorageGeneration& generation);
  [[nodiscard]] static std::expected<SqliteOrderStateStore, SqliteOrderStateStoreError> OpenExisting(const std::filesystem::path& path);

  ~SqliteOrderStateStore() override;

  SqliteOrderStateStore(const SqliteOrderStateStore&) = delete;
  SqliteOrderStateStore& operator=(const SqliteOrderStateStore&) = delete;

  SqliteOrderStateStore(SqliteOrderStateStore&& other) noexcept;
  SqliteOrderStateStore& operator=(SqliteOrderStateStore&& other) noexcept;

  [[nodiscard]] bool PersistTransition(const risk::OrderRegistryEntry& entry, OrderAuditEventType event_type) noexcept override;

  [[nodiscard]] bool LoadAll(std::vector<risk::OrderRegistryEntry>& entries) noexcept override;

  [[nodiscard]] bool LoadAuditEvents(std::vector<OrderAuditEvent>& events) noexcept override;

  [[nodiscard]] const StorageGeneration& Generation() const noexcept { return generation_; }

 private:
  [[nodiscard]] static std::expected<SqliteOrderStateStore, SqliteOrderStateStoreError> OpenWithMode(
      const std::filesystem::path& path, std::optional<StorageGeneration> generation, bool create_new);

  SqliteOrderStateStore(sqlite3* database, sqlite3_stmt* upsert_statement, sqlite3_stmt* load_statement,
                        StorageGeneration generation) noexcept;

  void Close() noexcept;

  sqlite3* database_{};
  sqlite3_stmt* upsert_statement_{};
  sqlite3_stmt* load_statement_{};
  bool healthy_{true};
  bool checkpoint_pending_{};
  StorageGeneration generation_;
};

}  // namespace botguard::execution
