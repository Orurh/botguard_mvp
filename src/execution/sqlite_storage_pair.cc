#include "sqlite_storage_pair.h"

#include <array>
#include <system_error>
#include <utility>

namespace botguard::execution {

namespace {

[[nodiscard]] std::expected<bool, SqliteStoragePairError> HasSqliteSidecar(const std::filesystem::path& path) noexcept {
  constexpr std::array suffixes{"-wal", "-shm", "-journal"};
  for (const auto* suffix : suffixes) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path.string() + suffix, error);
    if (error) {
      return std::unexpected(SqliteStoragePairError::kFilesystemInspectionFailed);
    }
    if (exists) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::expected<StoragePairPresence, SqliteStoragePairError> InspectStoragePair(const std::filesystem::path& order_path,
                                                                              const std::filesystem::path& runtime_path) noexcept {
  std::error_code error;
  const auto order_exists = std::filesystem::exists(order_path, error);
  if (error) {
    return std::unexpected(SqliteStoragePairError::kFilesystemInspectionFailed);
  }
  const auto runtime_exists = std::filesystem::exists(runtime_path, error);
  if (error) {
    return std::unexpected(SqliteStoragePairError::kFilesystemInspectionFailed);
  }
  if (order_exists != runtime_exists) {
    return std::unexpected(SqliteStoragePairError::kIncompletePair);
  }
  if (!order_exists) {
    const auto order_sidecar = HasSqliteSidecar(order_path);
    if (!order_sidecar) {
      return std::unexpected(order_sidecar.error());
    }
    const auto runtime_sidecar = HasSqliteSidecar(runtime_path);
    if (!runtime_sidecar) {
      return std::unexpected(runtime_sidecar.error());
    }
    if (*order_sidecar || *runtime_sidecar) {
      return std::unexpected(SqliteStoragePairError::kOrphanedStorageArtifacts);
    }
  }
  return order_exists ? StoragePairPresence::kExisting : StoragePairPresence::kAbsent;
}

std::expected<SqliteStoragePair, SqliteStoragePairError> SqliteStoragePair::CreateNew(const std::filesystem::path& order_path,
                                                                                      const std::filesystem::path& runtime_path,
                                                                                      const RuntimeBinding& binding,
                                                                                      const StorageGeneration& generation) {
  const auto presence = InspectStoragePair(order_path, runtime_path);
  if (!presence) {
    return std::unexpected(presence.error());
  }
  if (*presence != StoragePairPresence::kAbsent) {
    return std::unexpected(SqliteStoragePairError::kUnexpectedExistingPair);
  }
  auto order_state = SqliteOrderStateStore::CreateNew(order_path, generation);
  if (!order_state) {
    return std::unexpected(SqliteStoragePairError::kOrderStoreFailed);
  }
  auto runtime_state = SqliteRuntimeStateStore::CreateNew(runtime_path, binding, generation);
  if (!runtime_state) {
    return std::unexpected(SqliteStoragePairError::kRuntimeStoreFailed);
  }
  return SqliteStoragePair(std::move(*order_state), std::move(*runtime_state));
}

std::expected<SqliteStoragePair, SqliteStoragePairError> SqliteStoragePair::OpenExisting(const std::filesystem::path& order_path,
                                                                                         const std::filesystem::path& runtime_path,
                                                                                         const RuntimeBinding& binding) {
  const auto presence = InspectStoragePair(order_path, runtime_path);
  if (!presence) {
    return std::unexpected(presence.error());
  }
  if (*presence != StoragePairPresence::kExisting) {
    return std::unexpected(SqliteStoragePairError::kMissingPair);
  }
  auto order_state = SqliteOrderStateStore::OpenExisting(order_path);
  if (!order_state) {
    return std::unexpected(SqliteStoragePairError::kOrderStoreFailed);
  }
  auto runtime_state = SqliteRuntimeStateStore::OpenExisting(runtime_path, binding, order_state->Generation());
  if (!runtime_state) {
    return std::unexpected(SqliteStoragePairError::kRuntimeStoreFailed);
  }
  return SqliteStoragePair(std::move(*order_state), std::move(*runtime_state));
}

}  // namespace botguard::execution
