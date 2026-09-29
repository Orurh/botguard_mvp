#include "execution/sqlite_runtime_state_store.h"

#include <sqlite3.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace execution = botguard::execution;
namespace risk = botguard::risk;

[[nodiscard]] execution::RuntimeBinding Binding(char digit = 'a') {
  return execution::RuntimeBinding{.fingerprint = std::string(64, digit)};
}

[[nodiscard]] execution::StorageGeneration TestGeneration(char digit = '1') {
  return execution::StorageGeneration{.value = std::string(64, digit)};
}

[[nodiscard]] std::expected<execution::SqliteRuntimeStateStore, execution::SqliteRuntimeStateStoreCreateError> OpenStore(
    const std::filesystem::path& path, const execution::RuntimeBinding& binding,
    const execution::StorageGeneration& generation = TestGeneration()) {
  return std::filesystem::exists(path) ? execution::SqliteRuntimeStateStore::OpenExisting(path, binding, generation)
                                       : execution::SqliteRuntimeStateStore::CreateNew(path, binding, generation);
}

void ExecuteSql(const std::filesystem::path& path, const char* sql) {
  sqlite3* database{};
  const auto path_text = path.string();
  ASSERT_EQ(sqlite3_open_v2(path_text.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(database, sql, nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(database), SQLITE_OK);
}

class TemporaryRuntimeDatabase final {
 public:
  TemporaryRuntimeDatabase() {
    static std::atomic_uint64_t sequence{};
    path_ = std::filesystem::temp_directory_path() /
            ("botguard_runtime_state_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(sequence.fetch_add(1)) + ".db");
  }

  ~TemporaryRuntimeDatabase() {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(path_, error));
    static_cast<void>(std::filesystem::remove(path_.string() + "-wal", error));
    static_cast<void>(std::filesystem::remove(path_.string() + "-shm", error));
  }

  [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(SqliteRuntimeStateStoreTest, SameUtcDayCannotReplaceBaselineEquityAcrossRestart) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding());
    ASSERT_TRUE(store.has_value());
    const auto baseline = store->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(1'000));
    ASSERT_TRUE(baseline.has_value());
    EXPECT_EQ(*baseline, risk::Money::FromWholeUsd(1'000));
  }

  auto restarted = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(restarted.has_value());
  const auto baseline = restarted->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(700));
  ASSERT_TRUE(baseline.has_value());
  EXPECT_EQ(*baseline, risk::Money::FromWholeUsd(1'000));
}

TEST(SqliteRuntimeStateStoreTest, TradingDayContextReadDoesNotCreateOrRotateBaseline) {
  TemporaryRuntimeDatabase database;
  auto store = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(store.has_value());
  const auto absent = store->LoadTradingDayContext();
  ASSERT_TRUE(absent.has_value());
  EXPECT_FALSE(absent->has_value());

  ASSERT_TRUE(store->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(1'000)).has_value());
  const auto context = store->LoadTradingDayContext();
  ASSERT_TRUE(context.has_value());
  ASSERT_TRUE(context->has_value());
  EXPECT_EQ((*context)->utc_day, 20'000);
  EXPECT_EQ((*context)->baseline_equity, risk::Money::FromWholeUsd(1'000));
}

TEST(SqliteRuntimeStateStoreTest, NewUtcDayRotatesBaselineAtomicallyAndPersistsIt) {
  TemporaryRuntimeDatabase database;
  auto store = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(store.has_value());
  ASSERT_TRUE(store->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(1'000)).has_value());

  const auto rotated = store->ResolveDailyBaselineEquity(20'001, risk::Money::FromWholeUsd(800));
  ASSERT_TRUE(rotated.has_value());
  EXPECT_EQ(*rotated, risk::Money::FromWholeUsd(800));

  auto restarted = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(restarted.has_value());
  const auto loaded = restarted->ResolveDailyBaselineEquity(20'001, risk::Money::FromWholeUsd(900));
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, risk::Money::FromWholeUsd(800));
}

TEST(SqliteRuntimeStateStoreTest, UtcDayRegressionFailsClosedWithoutChangingBaseline) {
  TemporaryRuntimeDatabase database;
  auto store = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(store.has_value());
  ASSERT_TRUE(store->ResolveDailyBaselineEquity(20'001, risk::Money::FromWholeUsd(800)).has_value());

  const auto regressed = store->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(1'000));
  ASSERT_FALSE(regressed.has_value());
  EXPECT_EQ(regressed.error(), execution::DailyEquityBaselineError::kDayRegression);

  const auto unchanged = store->ResolveDailyBaselineEquity(20'001, risk::Money::FromWholeUsd(900));
  ASSERT_TRUE(unchanged.has_value());
  EXPECT_EQ(*unchanged, risk::Money::FromWholeUsd(800));
}

TEST(SqliteRuntimeStateStoreTest, InvalidBaselineInputIsRejected) {
  TemporaryRuntimeDatabase database;
  auto store = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(store.has_value());

  const auto invalid_day = store->ResolveDailyBaselineEquity(-1, risk::Money{});
  ASSERT_FALSE(invalid_day.has_value());
  EXPECT_EQ(invalid_day.error(), execution::DailyEquityBaselineError::kInvalidInput);
  const auto invalid_equity = store->ResolveDailyBaselineEquity(20'000, risk::Money{.micros = -1});
  ASSERT_FALSE(invalid_equity.has_value());
  EXPECT_EQ(invalid_equity.error(), execution::DailyEquityBaselineError::kInvalidInput);
}

TEST(SqliteRuntimeStateStoreTest, RuntimeBindingMismatchFailsClosed) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding('a'));
    ASSERT_TRUE(store.has_value());
    ASSERT_TRUE(store->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(100)).has_value());
  }

  const auto wrong_account = OpenStore(database.Path(), Binding('b'));
  ASSERT_FALSE(wrong_account.has_value());
  EXPECT_EQ(wrong_account.error(), execution::SqliteRuntimeStateStoreCreateError::kBindingMismatch);

  const auto original_account = OpenStore(database.Path(), Binding('a'));
  EXPECT_TRUE(original_account.has_value());
}

TEST(SqliteRuntimeStateStoreTest, InvalidRuntimeBindingIsRejectedWithoutCreatingDatabase) {
  TemporaryRuntimeDatabase database;
  const auto store = OpenStore(database.Path(), execution::RuntimeBinding{.fingerprint = "account-a"});
  ASSERT_FALSE(store.has_value());
  EXPECT_EQ(store.error(), execution::SqliteRuntimeStateStoreCreateError::kInvalidBinding);
  EXPECT_FALSE(std::filesystem::exists(database.Path()));
}

TEST(SqliteRuntimeStateStoreTest, VersionedDatabaseMissingBaselineTableIsRejected) {
  TemporaryRuntimeDatabase database;
  ExecuteSql(database.Path(), R"sql(
    CREATE TABLE storage_metadata (
      singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
      generation TEXT NOT NULL
    );
    INSERT INTO storage_metadata(singleton, generation)
    VALUES(1, '1111111111111111111111111111111111111111111111111111111111111111');
    CREATE TABLE runtime_binding (
      singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
      fingerprint TEXT NOT NULL
    );
    INSERT INTO runtime_binding(singleton, fingerprint)
    VALUES(1, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');
    PRAGMA user_version = 3;
  )sql");

  const auto store = OpenStore(database.Path(), Binding());
  ASSERT_FALSE(store.has_value());
  EXPECT_EQ(store.error(), execution::SqliteRuntimeStateStoreCreateError::kSchemaInitializationFailed);
}

TEST(SqliteRuntimeStateStoreTest, ExistingBaselineTriggerIsRejected) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding());
    ASSERT_TRUE(store.has_value());
  }
  ExecuteSql(database.Path(), R"sql(
    CREATE TRIGGER mutate_baseline
    BEFORE UPDATE ON daily_equity_baseline
    BEGIN
      SELECT 1;
    END;
  )sql");

  const auto reopened = OpenStore(database.Path(), Binding());
  ASSERT_FALSE(reopened.has_value());
  EXPECT_EQ(reopened.error(), execution::SqliteRuntimeStateStoreCreateError::kSchemaInitializationFailed);
}

TEST(SqliteRuntimeStateStoreTest, ExistingRuntimeControlTriggerIsRejected) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding());
    ASSERT_TRUE(store.has_value());
  }
  ExecuteSql(database.Path(), R"sql(
    CREATE TRIGGER suppress_operator_kill
    BEFORE UPDATE ON runtime_control
    BEGIN
      SELECT 1;
    END;
  )sql");

  const auto reopened = OpenStore(database.Path(), Binding());
  ASSERT_FALSE(reopened.has_value());
  EXPECT_EQ(reopened.error(), execution::SqliteRuntimeStateStoreCreateError::kSchemaInitializationFailed);
}

TEST(SqliteRuntimeStateStoreTest, CorruptBaselineRowFailsClosed) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding());
    ASSERT_TRUE(store.has_value());
    ASSERT_TRUE(store->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(1'000)).has_value());
  }
  ExecuteSql(database.Path(), R"sql(
    PRAGMA ignore_check_constraints = ON;
    UPDATE daily_equity_baseline SET opening_equity_micros = -1 WHERE singleton = 1;
  )sql");

  auto reopened = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(reopened.has_value());
  const auto baseline = reopened->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(1'000));
  ASSERT_FALSE(baseline.has_value());
  EXPECT_EQ(baseline.error(), execution::DailyEquityBaselineError::kCorruptState);
}

TEST(SqliteRuntimeStateStoreTest, BusyWriterDoesNotChangeBaseline) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding());
    ASSERT_TRUE(store.has_value());
    ASSERT_TRUE(store->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(1'000)).has_value());

    sqlite3* locking_connection{};
    const auto path = database.Path().string();
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &locking_connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(locking_connection, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);

    const auto blocked = store->ResolveDailyBaselineEquity(20'001, risk::Money::FromWholeUsd(800));
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), execution::DailyEquityBaselineError::kPersistenceFailed);

    ASSERT_EQ(sqlite3_exec(locking_connection, "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(locking_connection), SQLITE_OK);
  }

  auto restarted = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(restarted.has_value());
  const auto unchanged = restarted->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(700));
  ASSERT_TRUE(unchanged.has_value());
  EXPECT_EQ(*unchanged, risk::Money::FromWholeUsd(1'000));
}

TEST(SqliteRuntimeStateStoreTest, OperatorKillDefaultsFalseAndSurvivesRestart) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding());
    ASSERT_TRUE(store.has_value());
    const auto initial = store->OperatorKilled();
    ASSERT_TRUE(initial.has_value());
    EXPECT_FALSE(*initial);
    ASSERT_TRUE(store->SetOperatorKilled(true).has_value());
  }

  auto restarted = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(restarted.has_value());
  const auto killed = restarted->OperatorKilled();
  ASSERT_TRUE(killed.has_value());
  EXPECT_TRUE(*killed);
  ASSERT_TRUE(restarted->SetOperatorKilled(false).has_value());
  const auto cleared = restarted->OperatorKilled();
  ASSERT_TRUE(cleared.has_value());
  EXPECT_FALSE(*cleared);
}

TEST(SqliteRuntimeStateStoreTest, BusyWriterCannotPartiallyChangeOperatorKill) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding());
    ASSERT_TRUE(store.has_value());
    ASSERT_TRUE(store->SetOperatorKilled(true).has_value());

    sqlite3* locking_connection{};
    const auto path = database.Path().string();
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &locking_connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(locking_connection, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);
    const auto blocked = store->SetOperatorKilled(false);
    ASSERT_FALSE(blocked.has_value());
    EXPECT_EQ(blocked.error(), execution::RuntimeControlError::kPersistenceFailed);
    ASSERT_EQ(sqlite3_exec(locking_connection, "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(locking_connection), SQLITE_OK);
  }

  auto restarted = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(restarted.has_value());
  const auto killed = restarted->OperatorKilled();
  ASSERT_TRUE(killed.has_value());
  EXPECT_TRUE(*killed);
}

TEST(SqliteRuntimeStateStoreTest, CorruptOperatorKillFailsClosed) {
  TemporaryRuntimeDatabase database;
  {
    auto store = OpenStore(database.Path(), Binding());
    ASSERT_TRUE(store.has_value());
  }
  ExecuteSql(database.Path(), R"sql(
    PRAGMA ignore_check_constraints = ON;
    UPDATE runtime_control SET operator_killed = 7 WHERE singleton = 1;
  )sql");

  auto reopened = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(reopened.has_value());
  const auto killed = reopened->OperatorKilled();
  ASSERT_FALSE(killed.has_value());
  EXPECT_EQ(killed.error(), execution::RuntimeControlError::kCorruptState);
}

#if defined(__unix__) || defined(__APPLE__)
TEST(SqliteRuntimeStateStoreTest, BaselineSurvivesProcessExitWithoutDestructor) {
  TemporaryRuntimeDatabase database;
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    auto store = OpenStore(database.Path(), Binding());
    if (!store || !store->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(1'000))) {
      _exit(10);
    }
    _exit(0);
  }

  int status{};
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  auto reopened = OpenStore(database.Path(), Binding());
  ASSERT_TRUE(reopened.has_value());
  const auto baseline = reopened->ResolveDailyBaselineEquity(20'000, risk::Money::FromWholeUsd(700));
  ASSERT_TRUE(baseline.has_value());
  EXPECT_EQ(*baseline, risk::Money::FromWholeUsd(1'000));
}
#endif

}  // namespace
