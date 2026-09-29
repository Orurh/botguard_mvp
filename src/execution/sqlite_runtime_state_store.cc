#include "sqlite_runtime_state_store.h"

#include <sqlite3.h>

#include <array>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>

namespace botguard::execution {
namespace {

// The schema-v4 column name opening_equity_micros is retained for durable
// compatibility. Its V0 meaning is the first safe authoritative baseline
// observation for the UTC day, not historical equity at midnight.
constexpr const char* kCreateSchemaSql = R"sql(
CREATE TABLE storage_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  generation TEXT NOT NULL
);
CREATE TABLE runtime_binding (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  fingerprint TEXT NOT NULL
);
CREATE TABLE daily_equity_baseline (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  trading_day_utc INTEGER NOT NULL,
  opening_equity_micros INTEGER NOT NULL CHECK (opening_equity_micros >= 0)
);
CREATE TABLE runtime_control (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  operator_killed INTEGER NOT NULL CHECK (operator_killed IN (0, 1))
);
INSERT INTO runtime_control(singleton, operator_killed) VALUES(1, 0);
)sql";

[[nodiscard]] bool Execute(sqlite3* database, const char* sql) noexcept {
  return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

[[nodiscard]] std::optional<std::int64_t> ReadInteger(sqlite3* database, const char* sql) noexcept {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  std::optional<std::int64_t> value;
  if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER) {
    value = sqlite3_column_int64(statement, 0);
    if (sqlite3_step(statement) != SQLITE_DONE) {
      value.reset();
    }
  }
  if (sqlite3_finalize(statement) != SQLITE_OK) {
    value.reset();
  }
  return value;
}

[[nodiscard]] bool Configure(sqlite3* database) noexcept {
  sqlite3_stmt* journal{};
  if (sqlite3_prepare_v2(database, "PRAGMA journal_mode = WAL;", -1, &journal, nullptr) != SQLITE_OK) {
    return false;
  }
  const auto step = sqlite3_step(journal);
  const auto* journal_text = step == SQLITE_ROW ? reinterpret_cast<const char*>(sqlite3_column_text(journal, 0)) : nullptr;
  const auto journal_ok = journal_text != nullptr && std::string_view(journal_text) == "wal";
  const auto journal_finalize = sqlite3_finalize(journal);
  return journal_ok && journal_finalize == SQLITE_OK && sqlite3_wal_autocheckpoint(database, 0) == SQLITE_OK &&
         Execute(database, "PRAGMA synchronous = FULL;") && Execute(database, "PRAGMA busy_timeout = 0;");
}

[[nodiscard]] bool ValidateBaselineSchema(sqlite3* database) noexcept {
  constexpr std::array<std::string_view, 3> names{"singleton", "trading_day_utc", "opening_equity_micros"};
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "PRAGMA table_info(daily_equity_baseline);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  std::size_t index{};
  bool valid{true};
  int result{};
  while (valid && (result = sqlite3_step(statement)) == SQLITE_ROW) {
    if (index >= names.size() || sqlite3_column_type(statement, 1) != SQLITE_TEXT || sqlite3_column_type(statement, 2) != SQLITE_TEXT ||
        sqlite3_column_type(statement, 3) != SQLITE_INTEGER || sqlite3_column_type(statement, 5) != SQLITE_INTEGER) {
      valid = false;
      break;
    }
    const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
    valid = name != nullptr && type != nullptr && names[index] == name && std::string_view(type) == "INTEGER" &&
            sqlite3_column_int(statement, 3) == (index == 0 ? 0 : 1) && sqlite3_column_int(statement, 5) == (index == 0 ? 1 : 0);
    ++index;
  }
  valid = valid && index == names.size() && result == SQLITE_DONE;
  const auto finalized = sqlite3_finalize(statement) == SQLITE_OK;
  return valid && finalized;
}

[[nodiscard]] bool ValidateBindingSchema(sqlite3* database) noexcept {
  constexpr std::array<std::string_view, 2> names{"singleton", "fingerprint"};
  constexpr std::array<std::string_view, 2> types{"INTEGER", "TEXT"};
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "PRAGMA table_info(runtime_binding);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  std::size_t index{};
  bool valid{true};
  int result{};
  while (valid && (result = sqlite3_step(statement)) == SQLITE_ROW) {
    if (index >= names.size() || sqlite3_column_type(statement, 1) != SQLITE_TEXT || sqlite3_column_type(statement, 2) != SQLITE_TEXT ||
        sqlite3_column_type(statement, 3) != SQLITE_INTEGER || sqlite3_column_type(statement, 5) != SQLITE_INTEGER) {
      valid = false;
      break;
    }
    const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
    valid = name != nullptr && type != nullptr && names[index] == name && types[index] == type &&
            sqlite3_column_int(statement, 3) == (index == 0 ? 0 : 1) && sqlite3_column_int(statement, 5) == (index == 0 ? 1 : 0);
    ++index;
  }
  valid = valid && index == names.size() && result == SQLITE_DONE;
  return sqlite3_finalize(statement) == SQLITE_OK && valid;
}

[[nodiscard]] bool ValidateMetadataSchema(sqlite3* database) noexcept {
  constexpr std::array<std::string_view, 2> names{"singleton", "generation"};
  constexpr std::array<std::string_view, 2> types{"INTEGER", "TEXT"};
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "PRAGMA table_info(storage_metadata);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  std::size_t index{};
  bool valid{true};
  int result{};
  while (valid && (result = sqlite3_step(statement)) == SQLITE_ROW) {
    if (index >= names.size() || sqlite3_column_type(statement, 1) != SQLITE_TEXT || sqlite3_column_type(statement, 2) != SQLITE_TEXT ||
        sqlite3_column_type(statement, 3) != SQLITE_INTEGER || sqlite3_column_type(statement, 5) != SQLITE_INTEGER) {
      valid = false;
      break;
    }
    const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
    valid = name != nullptr && type != nullptr && names[index] == name && types[index] == type &&
            sqlite3_column_int(statement, 3) == (index == 0 ? 0 : 1) && sqlite3_column_int(statement, 5) == (index == 0 ? 1 : 0);
    ++index;
  }
  valid = valid && index == names.size() && result == SQLITE_DONE;
  return sqlite3_finalize(statement) == SQLITE_OK && valid;
}

[[nodiscard]] bool ValidateControlSchema(sqlite3* database) noexcept {
  constexpr std::array<std::string_view, 2> names{"singleton", "operator_killed"};
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "PRAGMA table_info(runtime_control);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  std::size_t index{};
  bool valid{true};
  int result{};
  while (valid && (result = sqlite3_step(statement)) == SQLITE_ROW) {
    if (index >= names.size() || sqlite3_column_type(statement, 1) != SQLITE_TEXT || sqlite3_column_type(statement, 2) != SQLITE_TEXT ||
        sqlite3_column_type(statement, 3) != SQLITE_INTEGER || sqlite3_column_type(statement, 5) != SQLITE_INTEGER) {
      valid = false;
      break;
    }
    const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
    valid = name != nullptr && type != nullptr && names[index] == name && std::string_view(type) == "INTEGER" &&
            sqlite3_column_int(statement, 3) == (index == 0 ? 0 : 1) && sqlite3_column_int(statement, 5) == (index == 0 ? 1 : 0);
    ++index;
  }
  valid = valid && index == names.size() && result == SQLITE_DONE;
  return sqlite3_finalize(statement) == SQLITE_OK && valid;
}

[[nodiscard]] bool ValidateSchema(sqlite3* database) noexcept {
  const auto tables =
      ReadInteger(database, "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\';");
  const auto triggers = ReadInteger(database,
                                    "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'trigger' AND "
                                    "tbl_name IN ('storage_metadata', 'runtime_binding', 'daily_equity_baseline', 'runtime_control');");
  return tables && *tables == 4 && triggers && *triggers == 0 && ValidateMetadataSchema(database) && ValidateBindingSchema(database) &&
         ValidateBaselineSchema(database) && ValidateControlSchema(database);
}

[[nodiscard]] bool IsValidFingerprint(std::string_view fingerprint) noexcept {
  return fingerprint.size() == 64 && fingerprint.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

[[nodiscard]] bool WriteBinding(sqlite3* database, std::string_view fingerprint) noexcept {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "INSERT INTO runtime_binding(singleton, fingerprint) VALUES(1, ?1);", -1, &statement, nullptr) !=
      SQLITE_OK) {
    return false;
  }
  const auto succeeded =
      sqlite3_bind_text(statement, 1, fingerprint.data(), static_cast<int>(fingerprint.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_DONE;
  return sqlite3_finalize(statement) == SQLITE_OK && succeeded;
}

[[nodiscard]] bool WriteGeneration(sqlite3* database, const StorageGeneration& generation) noexcept {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "INSERT INTO storage_metadata(singleton, generation) VALUES(1, ?1);", -1, &statement, nullptr) !=
      SQLITE_OK) {
    return false;
  }
  const auto succeeded =
      sqlite3_bind_text(statement, 1, generation.value.data(), static_cast<int>(generation.value.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_DONE;
  return sqlite3_finalize(statement) == SQLITE_OK && succeeded;
}

enum class GenerationCheck : std::uint8_t {
  kMatch,
  kMismatch,
  kCorrupt,
};

[[nodiscard]] GenerationCheck CheckGeneration(sqlite3* database, std::string_view expected) noexcept {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "SELECT singleton, generation FROM storage_metadata;", -1, &statement, nullptr) != SQLITE_OK) {
    return GenerationCheck::kCorrupt;
  }
  auto result = GenerationCheck::kCorrupt;
  if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
      sqlite3_column_int64(statement, 0) == 1 && sqlite3_column_type(statement, 1) == SQLITE_TEXT) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const auto bytes = sqlite3_column_bytes(statement, 1);
    if (text != nullptr && bytes >= 0) {
      const std::string_view stored(text, static_cast<std::size_t>(bytes));
      const auto valid = IsValidStorageGeneration(stored);
      const auto match = stored == expected;
      if (sqlite3_step(statement) == SQLITE_DONE) {
        result = valid ? (match ? GenerationCheck::kMatch : GenerationCheck::kMismatch) : GenerationCheck::kCorrupt;
      }
    }
  }
  if (sqlite3_finalize(statement) != SQLITE_OK) {
    return GenerationCheck::kCorrupt;
  }
  return result;
}

enum class BindingCheck : std::uint8_t {
  kMatch,
  kMismatch,
  kCorrupt,
};

[[nodiscard]] BindingCheck CheckBinding(sqlite3* database, std::string_view expected) noexcept {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "SELECT singleton, fingerprint FROM runtime_binding;", -1, &statement, nullptr) != SQLITE_OK) {
    return BindingCheck::kCorrupt;
  }
  auto result = BindingCheck::kCorrupt;
  if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
      sqlite3_column_int64(statement, 0) == 1 && sqlite3_column_type(statement, 1) == SQLITE_TEXT) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const auto bytes = sqlite3_column_bytes(statement, 1);
    if (text != nullptr && bytes >= 0) {
      const std::string_view stored(text, static_cast<std::size_t>(bytes));
      const auto valid = IsValidFingerprint(stored);
      const auto match = stored == expected;
      if (sqlite3_step(statement) == SQLITE_DONE) {
        result = valid ? (match ? BindingCheck::kMatch : BindingCheck::kMismatch) : BindingCheck::kCorrupt;
      }
    }
  }
  if (sqlite3_finalize(statement) != SQLITE_OK) {
    return BindingCheck::kCorrupt;
  }
  return result;
}

enum class SchemaInitialization : std::uint8_t {
  kReady,
  kInvalid,
  kBindingMismatch,
  kGenerationMismatch,
};

[[nodiscard]] SchemaInitialization InitializeNewSchema(sqlite3* database, std::string_view fingerprint,
                                                       const StorageGeneration& generation) noexcept {
  const auto version = ReadInteger(database, "PRAGMA user_version;");
  if (!version || *version != 0 || !Execute(database, "BEGIN IMMEDIATE;")) {
    return SchemaInitialization::kInvalid;
  }
  const auto ready = Execute(database, kCreateSchemaSql) && WriteGeneration(database, generation) && WriteBinding(database, fingerprint) &&
                     Execute(database, "PRAGMA user_version = 4;");
  if (!ready || !Execute(database, "COMMIT;")) {
    if (sqlite3_get_autocommit(database) == 0) {
      static_cast<void>(Execute(database, "ROLLBACK;"));
    }
    return SchemaInitialization::kInvalid;
  }
  return ValidateSchema(database) && CheckBinding(database, fingerprint) == BindingCheck::kMatch &&
                 CheckGeneration(database, generation.value) == GenerationCheck::kMatch
             ? SchemaInitialization::kReady
             : SchemaInitialization::kInvalid;
}

[[nodiscard]] SchemaInitialization ValidateExistingSchema(sqlite3* database, std::string_view fingerprint,
                                                          const StorageGeneration& generation) noexcept {
  const auto version = ReadInteger(database, "PRAGMA user_version;");
  if (!version || *version != 4 || !ValidateSchema(database)) {
    return SchemaInitialization::kInvalid;
  }
  const auto binding = CheckBinding(database, fingerprint);
  if (binding != BindingCheck::kMatch) {
    return binding == BindingCheck::kMismatch ? SchemaInitialization::kBindingMismatch : SchemaInitialization::kInvalid;
  }
  const auto stored_generation = CheckGeneration(database, generation.value);
  return stored_generation == GenerationCheck::kMatch
             ? SchemaInitialization::kReady
             : (stored_generation == GenerationCheck::kMismatch ? SchemaInitialization::kGenerationMismatch
                                                                : SchemaInitialization::kInvalid);
}

[[nodiscard]] bool Rollback(sqlite3* database) noexcept {
  return Execute(database, "ROLLBACK;") && sqlite3_get_autocommit(database) != 0;
}

struct StoredBaseline {
  std::int64_t day{};
  risk::Money equity{};
};

[[nodiscard]] std::expected<std::optional<StoredBaseline>, DailyEquityBaselineError> Load(sqlite3* database) noexcept {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "SELECT singleton, trading_day_utc, opening_equity_micros FROM daily_equity_baseline;", -1, &statement,
                         nullptr) != SQLITE_OK) {
    return std::unexpected(DailyEquityBaselineError::kCorruptState);
  }
  std::optional<StoredBaseline> baseline;
  const auto first = sqlite3_step(statement);
  if (first == SQLITE_ROW) {
    if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER || sqlite3_column_type(statement, 1) != SQLITE_INTEGER ||
        sqlite3_column_type(statement, 2) != SQLITE_INTEGER || sqlite3_column_int64(statement, 0) != 1) {
      static_cast<void>(sqlite3_finalize(statement));
      return std::unexpected(DailyEquityBaselineError::kCorruptState);
    }
    baseline =
        StoredBaseline{.day = sqlite3_column_int64(statement, 1), .equity = risk::Money{.micros = sqlite3_column_int64(statement, 2)}};
    if (baseline->day < 0 || baseline->equity.micros < 0 || sqlite3_step(statement) != SQLITE_DONE) {
      static_cast<void>(sqlite3_finalize(statement));
      return std::unexpected(DailyEquityBaselineError::kCorruptState);
    }
  } else if (first != SQLITE_DONE) {
    static_cast<void>(sqlite3_finalize(statement));
    return std::unexpected(DailyEquityBaselineError::kPersistenceFailed);
  }
  if (sqlite3_finalize(statement) != SQLITE_OK) {
    return std::unexpected(DailyEquityBaselineError::kPersistenceFailed);
  }
  return baseline;
}

[[nodiscard]] bool Write(sqlite3* database, bool insert, std::int64_t day, risk::Money equity) noexcept {
  sqlite3_stmt* statement{};
  const auto* sql = insert ? "INSERT INTO daily_equity_baseline(singleton, trading_day_utc, opening_equity_micros) VALUES(1, ?1, ?2);"
                           : "UPDATE daily_equity_baseline SET trading_day_utc = ?1, opening_equity_micros = ?2 WHERE singleton = 1;";
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  const auto succeeded = sqlite3_bind_int64(statement, 1, day) == SQLITE_OK &&
                         sqlite3_bind_int64(statement, 2, equity.micros) == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE &&
                         (insert || sqlite3_changes(database) == 1);
  return sqlite3_finalize(statement) == SQLITE_OK && succeeded;
}

}  // namespace

std::expected<SqliteRuntimeStateStore, SqliteRuntimeStateStoreCreateError> SqliteRuntimeStateStore::CreateNew(
    const std::filesystem::path& path, const RuntimeBinding& binding, const StorageGeneration& generation) {
  return OpenWithMode(path, binding, generation, true);
}

std::expected<SqliteRuntimeStateStore, SqliteRuntimeStateStoreCreateError> SqliteRuntimeStateStore::OpenExisting(
    const std::filesystem::path& path, const RuntimeBinding& binding, const StorageGeneration& expected_generation) {
  return OpenWithMode(path, binding, expected_generation, false);
}

std::expected<SqliteRuntimeStateStore, SqliteRuntimeStateStoreCreateError> SqliteRuntimeStateStore::OpenWithMode(
    const std::filesystem::path& path, const RuntimeBinding& binding, const StorageGeneration& generation, bool create_new) {
  if (!IsValidFingerprint(binding.fingerprint)) {
    return std::unexpected(SqliteRuntimeStateStoreCreateError::kInvalidBinding);
  }
  if (!IsValidStorageGeneration(generation.value)) {
    return std::unexpected(SqliteRuntimeStateStoreCreateError::kInvalidStorageGeneration);
  }
  sqlite3* database{};
  const auto path_text = path.string();
  const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | (create_new ? SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE : 0);
  if (sqlite3_open_v2(path_text.c_str(), &database, flags, nullptr) != SQLITE_OK) {
    if (database != nullptr) {
      static_cast<void>(sqlite3_close(database));
    }
    return std::unexpected(SqliteRuntimeStateStoreCreateError::kOpenFailed);
  }
  static_cast<void>(sqlite3_extended_result_codes(database, 1));
  if (!Configure(database)) {
    static_cast<void>(sqlite3_close(database));
    return std::unexpected(SqliteRuntimeStateStoreCreateError::kConfigurationFailed);
  }
  const auto initialized = create_new ? InitializeNewSchema(database, binding.fingerprint, generation)
                                      : ValidateExistingSchema(database, binding.fingerprint, generation);
  if (initialized != SchemaInitialization::kReady) {
    static_cast<void>(sqlite3_close(database));
    if (initialized == SchemaInitialization::kBindingMismatch) {
      return std::unexpected(SqliteRuntimeStateStoreCreateError::kBindingMismatch);
    }
    if (initialized == SchemaInitialization::kGenerationMismatch) {
      return std::unexpected(SqliteRuntimeStateStoreCreateError::kStorageGenerationMismatch);
    }
    return std::unexpected(SqliteRuntimeStateStoreCreateError::kSchemaInitializationFailed);
  }
  return SqliteRuntimeStateStore(database, generation);
}

SqliteRuntimeStateStore::~SqliteRuntimeStateStore() {
  Close();
}

SqliteRuntimeStateStore::SqliteRuntimeStateStore(SqliteRuntimeStateStore&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)), generation_(std::move(other.generation_)) {}

SqliteRuntimeStateStore& SqliteRuntimeStateStore::operator=(SqliteRuntimeStateStore&& other) noexcept {
  if (this != &other) {
    Close();
    database_ = std::exchange(other.database_, nullptr);
    generation_ = std::move(other.generation_);
  }
  return *this;
}

std::expected<risk::Money, DailyEquityBaselineError> SqliteRuntimeStateStore::ResolveDailyBaselineEquity(
    std::int64_t trading_day_utc, risk::Money proposed_baseline_equity) noexcept {
  if (database_ == nullptr || trading_day_utc < 0 || proposed_baseline_equity.micros < 0) {
    return std::unexpected(DailyEquityBaselineError::kInvalidInput);
  }
  if (!Execute(database_, "BEGIN IMMEDIATE;")) {
    return std::unexpected(DailyEquityBaselineError::kPersistenceFailed);
  }
  const auto stored = Load(database_);
  if (!stored) {
    if (!Rollback(database_)) {
      std::terminate();
    }
    return std::unexpected(stored.error());
  }
  if (*stored && (*stored)->day > trading_day_utc) {
    if (!Rollback(database_)) {
      std::terminate();
    }
    return std::unexpected(DailyEquityBaselineError::kDayRegression);
  }
  const auto result = *stored && (*stored)->day == trading_day_utc ? (*stored)->equity : proposed_baseline_equity;
  if ((!*stored || (*stored)->day < trading_day_utc) && !Write(database_, !stored->has_value(), trading_day_utc, result)) {
    if (!Rollback(database_)) {
      std::terminate();
    }
    return std::unexpected(DailyEquityBaselineError::kPersistenceFailed);
  }
  if (!Execute(database_, "COMMIT;")) {
    if (sqlite3_get_autocommit(database_) != 0 || !Rollback(database_)) {
      std::terminate();
    }
    return std::unexpected(DailyEquityBaselineError::kPersistenceFailed);
  }
  return result;
}

std::expected<std::optional<TradingDayContext>, DailyEquityBaselineError> SqliteRuntimeStateStore::LoadTradingDayContext() noexcept {
  if (database_ == nullptr) {
    return std::unexpected(DailyEquityBaselineError::kPersistenceFailed);
  }
  const auto stored = Load(database_);
  if (!stored) {
    return std::unexpected(stored.error());
  }
  if (!*stored) {
    return std::optional<TradingDayContext>{};
  }
  return std::optional<TradingDayContext>{TradingDayContext{
      .utc_day = (*stored)->day,
      .baseline_equity = (*stored)->equity,
  }};
}

std::expected<bool, RuntimeControlError> SqliteRuntimeStateStore::OperatorKilled() noexcept {
  if (database_ == nullptr) {
    return std::unexpected(RuntimeControlError::kPersistenceFailed);
  }
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database_, "SELECT singleton, operator_killed FROM runtime_control;", -1, &statement, nullptr) != SQLITE_OK) {
    return std::unexpected(RuntimeControlError::kPersistenceFailed);
  }
  std::optional<bool> killed;
  const auto first = sqlite3_step(statement);
  if (first == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER && sqlite3_column_int64(statement, 0) == 1 &&
      sqlite3_column_type(statement, 1) == SQLITE_INTEGER) {
    const auto raw = sqlite3_column_int64(statement, 1);
    if ((raw == 0 || raw == 1) && sqlite3_step(statement) == SQLITE_DONE) {
      killed = raw == 1;
    }
  }
  const auto finalized = sqlite3_finalize(statement) == SQLITE_OK;
  if (!finalized) {
    return std::unexpected(RuntimeControlError::kPersistenceFailed);
  }
  if (!killed) {
    return std::unexpected(first == SQLITE_BUSY || first == SQLITE_LOCKED ? RuntimeControlError::kPersistenceFailed
                                                                          : RuntimeControlError::kCorruptState);
  }
  return *killed;
}

std::expected<void, RuntimeControlError> SqliteRuntimeStateStore::SetOperatorKilled(bool killed) noexcept {
  if (database_ == nullptr || !Execute(database_, "BEGIN IMMEDIATE;")) {
    return std::unexpected(RuntimeControlError::kPersistenceFailed);
  }
  sqlite3_stmt* statement{};
  const auto prepared = sqlite3_prepare_v2(database_, "UPDATE runtime_control SET operator_killed = ?1 WHERE singleton = 1;", -1,
                                           &statement, nullptr) == SQLITE_OK;
  const auto updated = prepared && sqlite3_bind_int(statement, 1, killed ? 1 : 0) == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE &&
                       sqlite3_changes(database_) == 1;
  const auto finalized = statement == nullptr || sqlite3_finalize(statement) == SQLITE_OK;
  if (!updated || !finalized) {
    if (sqlite3_get_autocommit(database_) == 0 && (!Rollback(database_) || sqlite3_get_autocommit(database_) == 0)) {
      std::terminate();
    }
    return std::unexpected(RuntimeControlError::kPersistenceFailed);
  }
  if (!Execute(database_, "COMMIT;")) {
    if (sqlite3_get_autocommit(database_) != 0) {
      // Returning an error here could falsely claim that a durable admission
      // hold remains set even though the COMMIT may have succeeded.
      std::terminate();
    }
    if (!Rollback(database_) || sqlite3_get_autocommit(database_) == 0) {
      std::terminate();
    }
    return std::unexpected(RuntimeControlError::kPersistenceFailed);
  }
  return {};
}

void SqliteRuntimeStateStore::Close() noexcept {
  if (database_ != nullptr) {
    if (sqlite3_get_autocommit(database_) == 0) {
      static_cast<void>(Rollback(database_));
    }
    static_cast<void>(sqlite3_close(database_));
    database_ = nullptr;
  }
}

}  // namespace botguard::execution
