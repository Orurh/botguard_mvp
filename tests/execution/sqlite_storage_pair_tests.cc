#include "execution/sqlite_storage_pair.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace execution = botguard::execution;

class TemporaryStoragePair final {
 public:
  TemporaryStoragePair() {
    static std::atomic_uint64_t sequence{};
    order_path_ = std::filesystem::temp_directory_path() /
                  ("botguard_storage_pair_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                   std::to_string(sequence.fetch_add(1)) + ".db");
    runtime_path_ = order_path_.string() + ".runtime";
  }

  ~TemporaryStoragePair() {
    std::error_code error;
    for (const auto& path : {order_path_, runtime_path_}) {
      static_cast<void>(std::filesystem::remove(path, error));
      static_cast<void>(std::filesystem::remove(path.string() + "-wal", error));
      static_cast<void>(std::filesystem::remove(path.string() + "-shm", error));
      static_cast<void>(std::filesystem::remove(path.string() + "-journal", error));
    }
  }

  [[nodiscard]] const std::filesystem::path& OrderPath() const noexcept { return order_path_; }
  [[nodiscard]] const std::filesystem::path& RuntimePath() const noexcept { return runtime_path_; }

 private:
  std::filesystem::path order_path_;
  std::filesystem::path runtime_path_;
};

[[nodiscard]] execution::RuntimeBinding Binding() {
  return execution::RuntimeBinding{.fingerprint = std::string(64, 'a')};
}

[[nodiscard]] execution::StorageGeneration Generation(char digit = '1') {
  return execution::StorageGeneration{.value = std::string(64, digit)};
}

TEST(SqliteStoragePairTest, NewPairPersistsOneSharedGenerationAndReopens) {
  TemporaryStoragePair paths;
  {
    auto pair = execution::SqliteStoragePair::CreateNew(paths.OrderPath(), paths.RuntimePath(), Binding(), Generation());
    ASSERT_TRUE(pair.has_value());
    EXPECT_EQ(pair->OrderState().Generation(), Generation());
    EXPECT_EQ(pair->RuntimeState().Generation(), Generation());
  }

  auto reopened = execution::SqliteStoragePair::OpenExisting(paths.OrderPath(), paths.RuntimePath(), Binding());
  ASSERT_TRUE(reopened.has_value());
  EXPECT_EQ(reopened->OrderState().Generation(), Generation());
  EXPECT_EQ(reopened->RuntimeState().Generation(), Generation());
}

TEST(SqliteStoragePairTest, MissingRuntimeDatabaseFailsClosed) {
  TemporaryStoragePair paths;
  auto order = execution::SqliteOrderStateStore::CreateNew(paths.OrderPath(), Generation());
  ASSERT_TRUE(order.has_value());

  const auto presence = execution::InspectStoragePair(paths.OrderPath(), paths.RuntimePath());
  ASSERT_FALSE(presence.has_value());
  EXPECT_EQ(presence.error(), execution::SqliteStoragePairError::kIncompletePair);
  const auto pair = execution::SqliteStoragePair::OpenExisting(paths.OrderPath(), paths.RuntimePath(), Binding());
  ASSERT_FALSE(pair.has_value());
  EXPECT_EQ(pair.error(), execution::SqliteStoragePairError::kIncompletePair);
}

TEST(SqliteStoragePairTest, MissingOrderDatabaseFailsClosed) {
  TemporaryStoragePair paths;
  auto runtime = execution::SqliteRuntimeStateStore::CreateNew(paths.RuntimePath(), Binding(), Generation());
  ASSERT_TRUE(runtime.has_value());

  const auto pair = execution::SqliteStoragePair::OpenExisting(paths.OrderPath(), paths.RuntimePath(), Binding());
  ASSERT_FALSE(pair.has_value());
  EXPECT_EQ(pair.error(), execution::SqliteStoragePairError::kIncompletePair);
}

TEST(SqliteStoragePairTest, SubstitutedRuntimeGenerationFailsClosed) {
  TemporaryStoragePair paths;
  {
    auto order = execution::SqliteOrderStateStore::CreateNew(paths.OrderPath(), Generation('1'));
    ASSERT_TRUE(order.has_value());
  }
  {
    auto runtime = execution::SqliteRuntimeStateStore::CreateNew(paths.RuntimePath(), Binding(), Generation('2'));
    ASSERT_TRUE(runtime.has_value());
  }

  const auto pair = execution::SqliteStoragePair::OpenExisting(paths.OrderPath(), paths.RuntimePath(), Binding());
  ASSERT_FALSE(pair.has_value());
  EXPECT_EQ(pair.error(), execution::SqliteStoragePairError::kRuntimeStoreFailed);
}

class SqliteStorageSidecarTest : public testing::TestWithParam<std::pair<bool, std::string_view>> {};

TEST_P(SqliteStorageSidecarTest, OrphanedArtifactPreventsNewStorageBootstrap) {
  TemporaryStoragePair paths;
  const auto& [runtime_sidecar, suffix] = GetParam();
  const auto& database_path = runtime_sidecar ? paths.RuntimePath() : paths.OrderPath();
  {
    std::ofstream artifact(database_path.string() + std::string(suffix), std::ios::binary);
    ASSERT_TRUE(artifact.good());
    artifact << "orphan";
  }

  const auto presence = execution::InspectStoragePair(paths.OrderPath(), paths.RuntimePath());
  ASSERT_FALSE(presence.has_value());
  EXPECT_EQ(presence.error(), execution::SqliteStoragePairError::kOrphanedStorageArtifacts);

  const auto pair = execution::SqliteStoragePair::CreateNew(paths.OrderPath(), paths.RuntimePath(), Binding(), Generation());
  ASSERT_FALSE(pair.has_value());
  EXPECT_EQ(pair.error(), execution::SqliteStoragePairError::kOrphanedStorageArtifacts);
}

INSTANTIATE_TEST_SUITE_P(OrderAndRuntimeArtifacts, SqliteStorageSidecarTest,
                         testing::Values(std::pair{false, std::string_view{"-wal"}}, std::pair{false, std::string_view{"-shm"}},
                                         std::pair{false, std::string_view{"-journal"}}, std::pair{true, std::string_view{"-wal"}},
                                         std::pair{true, std::string_view{"-shm"}}, std::pair{true, std::string_view{"-journal"}}));

TEST(SqliteStoragePairTest, RuntimeCreationFailureLeavesPairFailClosed) {
  TemporaryStoragePair paths;
  const auto unavailable_runtime_path = paths.RuntimePath() / "missing-parent" / "runtime.db";

  const auto creation = execution::SqliteStoragePair::CreateNew(paths.OrderPath(), unavailable_runtime_path, Binding(), Generation());
  ASSERT_FALSE(creation.has_value());
  EXPECT_EQ(creation.error(), execution::SqliteStoragePairError::kRuntimeStoreFailed);

  const auto presence = execution::InspectStoragePair(paths.OrderPath(), unavailable_runtime_path);
  ASSERT_FALSE(presence.has_value());
  EXPECT_EQ(presence.error(), execution::SqliteStoragePairError::kIncompletePair);
}

}  // namespace
