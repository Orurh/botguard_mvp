#pragma once

#include "sqlite_order_state_store.h"
#include "sqlite_runtime_state_store.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <utility>

namespace botguard::execution {

enum class StoragePairPresence : std::uint8_t {
  kAbsent,
  kExisting,
};

enum class SqliteStoragePairError : std::uint8_t {
  kFilesystemInspectionFailed,
  kIncompletePair,
  kOrphanedStorageArtifacts,
  kUnexpectedExistingPair,
  kMissingPair,
  kOrderStoreFailed,
  kRuntimeStoreFailed,
};

[[nodiscard]] std::expected<StoragePairPresence, SqliteStoragePairError> InspectStoragePair(
    const std::filesystem::path& order_path, const std::filesystem::path& runtime_path) noexcept;

class SqliteStoragePair final {
 public:
  [[nodiscard]] static std::expected<SqliteStoragePair, SqliteStoragePairError> CreateNew(const std::filesystem::path& order_path,
                                                                                          const std::filesystem::path& runtime_path,
                                                                                          const RuntimeBinding& binding,
                                                                                          const StorageGeneration& generation);
  [[nodiscard]] static std::expected<SqliteStoragePair, SqliteStoragePairError> OpenExisting(const std::filesystem::path& order_path,
                                                                                             const std::filesystem::path& runtime_path,
                                                                                             const RuntimeBinding& binding);

  [[nodiscard]] SqliteOrderStateStore& OrderState() noexcept { return order_state_; }
  [[nodiscard]] SqliteRuntimeStateStore& RuntimeState() noexcept { return runtime_state_; }

 private:
  SqliteStoragePair(SqliteOrderStateStore order_state, SqliteRuntimeStateStore runtime_state) noexcept
      : order_state_(std::move(order_state)), runtime_state_(std::move(runtime_state)) {}

  SqliteOrderStateStore order_state_;
  SqliteRuntimeStateStore runtime_state_;
};

}  // namespace botguard::execution
