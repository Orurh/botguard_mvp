#include "sqlite_order_state_store.h"
#include "risk/order_notional.h"

#include <sqlite3.h>

#include <array>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace botguard::execution {
namespace {

constexpr const char* kCreateSchemaSql = R"sql(
CREATE TABLE storage_metadata (
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  generation TEXT NOT NULL
);
CREATE TABLE order_state (
  client_order_id INTEGER PRIMARY KEY,
  state INTEGER NOT NULL,
  market_id INTEGER NOT NULL,
  reserved_notional INTEGER NOT NULL,
  reservation_active INTEGER NOT NULL,
  strategy_id INTEGER NOT NULL,
  side INTEGER NOT NULL,
  price INTEGER NOT NULL,
  quantity INTEGER NOT NULL,
  venue_order_id TEXT
);
CREATE TABLE order_event (
  sequence INTEGER PRIMARY KEY AUTOINCREMENT,
  client_order_id INTEGER NOT NULL,
  event_type INTEGER NOT NULL CHECK (event_type BETWEEN 1 AND 11),
  resulting_state INTEGER NOT NULL CHECK (resulting_state BETWEEN 0 AND 6),
  venue_order_id TEXT,
  FOREIGN KEY (client_order_id) REFERENCES order_state(client_order_id),
  UNIQUE (client_order_id, event_type)
);
)sql";

constexpr const char* kUpsertSql = R"sql(
INSERT INTO order_state (
  client_order_id,
  state,
  market_id,
  reserved_notional,
  reservation_active,
  strategy_id,
  side,
  price,
  quantity,
  venue_order_id
)
VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)
ON CONFLICT(client_order_id) DO UPDATE SET
  state = excluded.state,
  market_id = excluded.market_id,
  reserved_notional = excluded.reserved_notional,
  reservation_active = excluded.reservation_active,
  strategy_id = excluded.strategy_id,
  side = excluded.side,
  price = excluded.price,
  quantity = excluded.quantity,
  venue_order_id = excluded.venue_order_id;
)sql";

constexpr const char* kLoadAllSql = R"sql(
SELECT
  client_order_id,
  state,
  market_id,
  reserved_notional,
  reservation_active,
  strategy_id,
  side,
  price,
  quantity,
  venue_order_id
FROM order_state
ORDER BY client_order_id ASC;
)sql";

constexpr const char* kFindEventSql = R"sql(
SELECT
  event.resulting_state,
  event.venue_order_id,
  state.client_order_id,
  state.market_id,
  state.reserved_notional,
  state.strategy_id,
  state.side,
  state.price,
  state.quantity
FROM order_event AS event
JOIN order_state AS state ON state.client_order_id = event.client_order_id
WHERE event.client_order_id = ?1 AND event.event_type = ?2;
)sql";

constexpr const char* kInsertEventSql = R"sql(
INSERT INTO order_event (client_order_id, event_type, resulting_state, venue_order_id)
VALUES (?1, ?2, ?3, ?4);
)sql";

constexpr const char* kLoadAuditEventsSql = R"sql(
SELECT sequence, client_order_id, event_type, resulting_state, venue_order_id
FROM order_event
ORDER BY sequence ASC;
)sql";

[[nodiscard]] bool Execute(sqlite3* database, const char* sql) noexcept {
  return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

[[nodiscard]] bool SqliteTextEquals(sqlite3_stmt* statement, int column, std::string_view expected,
                                    bool ignore_ascii_case = false) noexcept {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
    return false;
  }

  const auto* value = sqlite3_column_text(statement, column);
  const auto byte_count = sqlite3_column_bytes(statement, column);

  if (value == nullptr || byte_count < 0 || static_cast<std::size_t>(byte_count) != expected.size()) {
    return false;
  }

  for (std::size_t index = 0; index < expected.size(); ++index) {
    auto actual = static_cast<char>(value[index]);
    auto wanted = expected[index];

    if (ignore_ascii_case) {
      if (actual >= 'A' && actual <= 'Z') {
        actual = static_cast<char>(actual - 'A' + 'a');
      }

      if (wanted >= 'A' && wanted <= 'Z') {
        wanted = static_cast<char>(wanted - 'A' + 'a');
      }
    }

    if (actual != wanted) {
      return false;
    }
  }

  return true;
}

void CloseResources(sqlite3* database, sqlite3_stmt* upsert_statement, sqlite3_stmt* load_statement) noexcept {
  if (load_statement != nullptr) {
    static_cast<void>(sqlite3_finalize(load_statement));
  }

  if (upsert_statement != nullptr) {
    static_cast<void>(sqlite3_finalize(upsert_statement));
  }

  if (database != nullptr) {
    static_cast<void>(sqlite3_close(database));
  }
}

[[nodiscard]] bool SetJournalModeWal(sqlite3* database) noexcept {
  sqlite3_stmt* statement{};

  if (sqlite3_prepare_v2(database, "PRAGMA journal_mode = WAL;", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }

  const auto step_result = sqlite3_step(statement);
  bool is_wal{};

  is_wal = step_result == SQLITE_ROW && SqliteTextEquals(statement, 0, "wal", true);

  const auto finalize_result = sqlite3_finalize(statement);
  return is_wal && finalize_result == SQLITE_OK;
}

[[nodiscard]] bool ReadIntegerPragma(sqlite3* database, const char* sql, int expected) noexcept {
  sqlite3_stmt* statement{};

  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }

  const auto first_step = sqlite3_step(statement);
  bool matches{};

  if (first_step == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER) {
    matches = sqlite3_column_int(statement, 0) == expected && sqlite3_step(statement) == SQLITE_DONE;
  }

  const auto finalize_result = sqlite3_finalize(statement);
  return matches && finalize_result == SQLITE_OK;
}

[[nodiscard]] std::optional<int> ReadIntegerValue(sqlite3* database, const char* sql) noexcept {
  sqlite3_stmt* statement{};

  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }

  const auto first_step = sqlite3_step(statement);
  std::optional<int> value;

  if (first_step == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER) {
    value = sqlite3_column_int(statement, 0);

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
  if (!SetJournalModeWal(database)) {
    return false;
  }

  // SQLite's default WAL auto-checkpoint runs from the commit hook. A
  // checkpoint error may therefore be reported by COMMIT after the
  // transaction itself has committed, which cannot be represented by
  // OrderStateStore's boolean result. Keep checkpointing outside the next
  // PersistTransition()
  // so false always means this call did not replace durable state.
  if (sqlite3_wal_autocheckpoint(database, 0) != SQLITE_OK) {
    return false;
  }

  if (!Execute(database, "PRAGMA synchronous = FULL;") || !ReadIntegerPragma(database, "PRAGMA synchronous;", 2)) {
    return false;
  }

  if (!Execute(database, "PRAGMA foreign_keys = ON;") || !ReadIntegerPragma(database, "PRAGMA foreign_keys;", 1)) {
    return false;
  }

  if (sqlite3_busy_timeout(database, 0) != SQLITE_OK || !ReadIntegerPragma(database, "PRAGMA busy_timeout;", 0)) {
    return false;
  }

  return true;
}

struct ExpectedColumn {
  std::string_view name;
  std::string_view type;
  int not_null;
  int primary_key;
};

[[nodiscard]] bool ValidateOrderSchema(sqlite3* database) noexcept {
  constexpr std::array<ExpectedColumn, 10> expected_columns{
      ExpectedColumn{.name = "client_order_id", .type = "INTEGER", .not_null = 0, .primary_key = 1},
      ExpectedColumn{.name = "state", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "market_id", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "reserved_notional", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "reservation_active", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "strategy_id", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "side", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "price", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "quantity", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "venue_order_id", .type = "TEXT", .not_null = 0, .primary_key = 0},
  };

  sqlite3_stmt* statement{};

  if (sqlite3_prepare_v2(database, "PRAGMA table_info(order_state);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }

  std::size_t column_index{};
  bool valid{true};
  int step_result{};

  while (valid && (step_result = sqlite3_step(statement)) == SQLITE_ROW) {
    if (column_index >= expected_columns.size() || sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(statement, 1) != SQLITE_TEXT || sqlite3_column_type(statement, 2) != SQLITE_TEXT ||
        sqlite3_column_type(statement, 3) != SQLITE_INTEGER || sqlite3_column_type(statement, 4) != SQLITE_NULL ||
        sqlite3_column_type(statement, 5) != SQLITE_INTEGER) {
      valid = false;
      break;
    }

    const auto& expected = expected_columns[column_index];
    valid = sqlite3_column_int(statement, 0) == static_cast<int>(column_index) && SqliteTextEquals(statement, 1, expected.name) &&
            SqliteTextEquals(statement, 2, expected.type, true) && sqlite3_column_int(statement, 3) == expected.not_null &&
            sqlite3_column_int(statement, 5) == expected.primary_key;

    ++column_index;
  }

  valid = valid && column_index == expected_columns.size() && step_result == SQLITE_DONE;

  const auto finalize_result = sqlite3_finalize(statement);

  if (!valid || finalize_result != SQLITE_OK) {
    return false;
  }

  const auto trigger_count =
      ReadIntegerValue(database, "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'trigger' AND tbl_name = 'order_state';");

  return trigger_count && *trigger_count == 0;
}

[[nodiscard]] bool ValidateAuditSchema(sqlite3* database) noexcept {
  constexpr std::array<ExpectedColumn, 5> expected_columns{
      ExpectedColumn{.name = "sequence", .type = "INTEGER", .not_null = 0, .primary_key = 1},
      ExpectedColumn{.name = "client_order_id", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "event_type", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "resulting_state", .type = "INTEGER", .not_null = 1, .primary_key = 0},
      ExpectedColumn{.name = "venue_order_id", .type = "TEXT", .not_null = 0, .primary_key = 0},
  };
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "PRAGMA table_info(order_event);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  std::size_t index{};
  bool valid{true};
  int step{};
  while (valid && (step = sqlite3_step(statement)) == SQLITE_ROW) {
    if (index >= expected_columns.size() || sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(statement, 1) != SQLITE_TEXT || sqlite3_column_type(statement, 2) != SQLITE_TEXT ||
        sqlite3_column_type(statement, 3) != SQLITE_INTEGER || sqlite3_column_type(statement, 4) != SQLITE_NULL ||
        sqlite3_column_type(statement, 5) != SQLITE_INTEGER) {
      valid = false;
      break;
    }
    const auto& expected = expected_columns[index];
    valid = sqlite3_column_int(statement, 0) == static_cast<int>(index) && SqliteTextEquals(statement, 1, expected.name) &&
            SqliteTextEquals(statement, 2, expected.type, true) && sqlite3_column_int(statement, 3) == expected.not_null &&
            sqlite3_column_int(statement, 5) == expected.primary_key;
    ++index;
  }
  valid = valid && index == expected_columns.size() && step == SQLITE_DONE && sqlite3_finalize(statement) == SQLITE_OK;
  if (!valid) {
    return false;
  }

  if (sqlite3_prepare_v2(database, "PRAGMA index_list(order_event);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  valid = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 1) == SQLITE_TEXT &&
          SqliteTextEquals(statement, 1, "sqlite_autoindex_order_event_1") && sqlite3_column_type(statement, 2) == SQLITE_INTEGER &&
          sqlite3_column_int(statement, 2) == 1 && sqlite3_column_type(statement, 3) == SQLITE_TEXT &&
          SqliteTextEquals(statement, 3, "u") && sqlite3_column_type(statement, 4) == SQLITE_INTEGER &&
          sqlite3_column_int(statement, 4) == 0 && sqlite3_step(statement) == SQLITE_DONE;
  valid = sqlite3_finalize(statement) == SQLITE_OK && valid;
  if (!valid || sqlite3_prepare_v2(database, "PRAGMA index_info(sqlite_autoindex_order_event_1);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  valid = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 0 && sqlite3_column_int(statement, 1) == 1 &&
          SqliteTextEquals(statement, 2, "client_order_id") && sqlite3_step(statement) == SQLITE_ROW &&
          sqlite3_column_int(statement, 0) == 1 && sqlite3_column_int(statement, 1) == 2 && SqliteTextEquals(statement, 2, "event_type") &&
          sqlite3_step(statement) == SQLITE_DONE;
  valid = sqlite3_finalize(statement) == SQLITE_OK && valid;
  if (!valid || sqlite3_prepare_v2(database, "PRAGMA foreign_key_list(order_event);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  valid = sqlite3_step(statement) == SQLITE_ROW && SqliteTextEquals(statement, 2, "order_state") &&
          SqliteTextEquals(statement, 3, "client_order_id") && SqliteTextEquals(statement, 4, "client_order_id") &&
          SqliteTextEquals(statement, 5, "NO ACTION") && SqliteTextEquals(statement, 6, "NO ACTION") &&
          sqlite3_step(statement) == SQLITE_DONE;
  valid = sqlite3_finalize(statement) == SQLITE_OK && valid;

  const auto autoincrement =
      ReadIntegerValue(database,
                       "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name = 'order_event' AND lower(sql) LIKE "
                       "'%sequence integer primary key autoincrement%';");
  const auto trigger_count =
      ReadIntegerValue(database, "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'trigger' AND tbl_name = 'order_event';");
  return valid && autoincrement && *autoincrement == 1 && trigger_count && *trigger_count == 0;
}

[[nodiscard]] bool ValidateMetadataSchema(sqlite3* database) noexcept {
  constexpr std::array<ExpectedColumn, 2> expected_columns{
      ExpectedColumn{.name = "singleton", .type = "INTEGER", .not_null = 0, .primary_key = 1},
      ExpectedColumn{.name = "generation", .type = "TEXT", .not_null = 1, .primary_key = 0},
  };
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "PRAGMA table_info(storage_metadata);", -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  std::size_t index{};
  bool valid{true};
  int step{};
  while (valid && (step = sqlite3_step(statement)) == SQLITE_ROW) {
    if (index >= expected_columns.size() || sqlite3_column_type(statement, 0) != SQLITE_INTEGER ||
        sqlite3_column_type(statement, 1) != SQLITE_TEXT || sqlite3_column_type(statement, 2) != SQLITE_TEXT ||
        sqlite3_column_type(statement, 3) != SQLITE_INTEGER || sqlite3_column_type(statement, 4) != SQLITE_NULL ||
        sqlite3_column_type(statement, 5) != SQLITE_INTEGER) {
      valid = false;
      break;
    }
    const auto& expected = expected_columns[index];
    valid = sqlite3_column_int(statement, 0) == static_cast<int>(index) && SqliteTextEquals(statement, 1, expected.name) &&
            SqliteTextEquals(statement, 2, expected.type, true) && sqlite3_column_int(statement, 3) == expected.not_null &&
            sqlite3_column_int(statement, 5) == expected.primary_key;
    ++index;
  }
  valid = valid && index == expected_columns.size() && step == SQLITE_DONE;
  return sqlite3_finalize(statement) == SQLITE_OK && valid;
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

[[nodiscard]] std::optional<StorageGeneration> ReadGeneration(sqlite3* database) {
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database, "SELECT singleton, generation FROM storage_metadata;", -1, &statement, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  std::optional<StorageGeneration> generation;
  if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
      sqlite3_column_int64(statement, 0) == 1 && sqlite3_column_type(statement, 1) == SQLITE_TEXT) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    const auto bytes = sqlite3_column_bytes(statement, 1);
    if (text != nullptr && bytes >= 0) {
      std::string value(text, static_cast<std::size_t>(bytes));
      if (IsValidStorageGeneration(value) && sqlite3_step(statement) == SQLITE_DONE) {
        generation = StorageGeneration{.value = std::move(value)};
      }
    }
  }
  if (sqlite3_finalize(statement) != SQLITE_OK) {
    generation.reset();
  }
  return generation;
}

[[nodiscard]] bool ValidateCompleteSchema(sqlite3* database) noexcept {
  const auto table_count = ReadIntegerValue(
      database, "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name IN ('storage_metadata', 'order_state', 'order_event');");
  const auto trigger_count = ReadIntegerValue(database,
                                              "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'trigger' AND "
                                              "tbl_name IN ('storage_metadata', 'order_state', 'order_event');");
  return table_count && *table_count == 3 && trigger_count && *trigger_count == 0 && ValidateMetadataSchema(database) &&
         ValidateOrderSchema(database) && ValidateAuditSchema(database);
}

[[nodiscard]] std::optional<bool> OrderStateTableExists(sqlite3* database) noexcept {
  const auto count = ReadIntegerValue(database, "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name = 'order_state';");

  if (!count || (*count != 0 && *count != 1)) {
    return std::nullopt;
  }

  return *count == 1;
}

[[nodiscard]] bool InitializeNewSchema(sqlite3* database, const StorageGeneration& generation) noexcept {
  const auto version = ReadIntegerValue(database, "PRAGMA user_version;");
  if (!version || *version != 0 || !Execute(database, "BEGIN IMMEDIATE;")) {
    return false;
  }
  const auto table_exists = OrderStateTableExists(database);
  const auto version_written = table_exists && !*table_exists && Execute(database, kCreateSchemaSql) &&
                               WriteGeneration(database, generation) && Execute(database, "PRAGMA user_version = 4;");
  if (!version_written || !Execute(database, "COMMIT;")) {
    if (sqlite3_get_autocommit(database) == 0) {
      static_cast<void>(Execute(database, "ROLLBACK;"));
    }

    return false;
  }
  return ReadIntegerPragma(database, "PRAGMA user_version;", 4) && ValidateCompleteSchema(database);
}

[[nodiscard]] std::optional<StorageGeneration> ValidateExistingSchema(sqlite3* database) {
  const auto version = ReadIntegerValue(database, "PRAGMA user_version;");
  if (!version || *version != 4 || !ValidateCompleteSchema(database)) {
    return std::nullopt;
  }
  return ReadGeneration(database);
}

[[nodiscard]] constexpr std::optional<sqlite3_int64> EncodeSide(risk::Side side) noexcept {
  switch (side) {
    case risk::Side::kBuy:
      return 0;
    case risk::Side::kSell:
      return 1;
  }

  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<risk::Side> DecodeSide(sqlite3_int64 value) noexcept {
  if (value == 0) {
    return risk::Side::kBuy;
  }
  if (value == 1) {
    return risk::Side::kSell;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<sqlite3_int64> EncodeState(risk::OrderState state) noexcept {
  switch (state) {
    case risk::OrderState::kPendingSubmit:
      return 0;
    case risk::OrderState::kOpen:
      return 1;
    case risk::OrderState::kFilled:
      return 2;
    case risk::OrderState::kCancelled:
      return 3;
    case risk::OrderState::kRejected:
      return 4;
    case risk::OrderState::kAbortedBeforeSubmit:
      return 5;
    case risk::OrderState::kUnknown:
      return 6;
  }

  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<risk::OrderState> DecodeState(sqlite3_int64 value) noexcept {
  switch (value) {
    case 0:
      return risk::OrderState::kPendingSubmit;
    case 1:
      return risk::OrderState::kOpen;
    case 2:
      return risk::OrderState::kFilled;
    case 3:
      return risk::OrderState::kCancelled;
    case 4:
      return risk::OrderState::kRejected;
    case 5:
      return risk::OrderState::kAbortedBeforeSubmit;
    case 6:
      return risk::OrderState::kUnknown;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] constexpr std::optional<sqlite3_int64> EncodeEventType(OrderAuditEventType type) noexcept {
  const auto value = static_cast<sqlite3_int64>(type);
  return value >= 1 && value <= 11 ? std::optional<sqlite3_int64>{value} : std::nullopt;
}

[[nodiscard]] constexpr std::optional<OrderAuditEventType> DecodeEventType(sqlite3_int64 value) noexcept {
  return value >= 1 && value <= 11 ? std::optional<OrderAuditEventType>{static_cast<OrderAuditEventType>(value)} : std::nullopt;
}

[[nodiscard]] constexpr std::optional<risk::OrderState> ResultingStateFor(OrderAuditEventType type) noexcept {
  switch (type) {
    case OrderAuditEventType::kIntentPendingDurable:
      return risk::OrderState::kPendingSubmit;
    case OrderAuditEventType::kAbortedBeforeSubmit:
      return risk::OrderState::kAbortedBeforeSubmit;
    case OrderAuditEventType::kVenueOpen:
    case OrderAuditEventType::kReconciledOpen:
      return risk::OrderState::kOpen;
    case OrderAuditEventType::kVenueFilled:
    case OrderAuditEventType::kReconciledFilled:
    case OrderAuditEventType::kExposureReconciled:
      return risk::OrderState::kFilled;
    case OrderAuditEventType::kVenueRejected:
    case OrderAuditEventType::kReconciledRejected:
      return risk::OrderState::kRejected;
    case OrderAuditEventType::kVenueUnknown:
      return risk::OrderState::kUnknown;
    case OrderAuditEventType::kReconciledCancelled:
      return risk::OrderState::kCancelled;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr bool StateAndReservationAreValid(risk::OrderState state, bool reservation_active) noexcept {
  switch (state) {
    case risk::OrderState::kPendingSubmit:
    case risk::OrderState::kOpen:
    case risk::OrderState::kUnknown:
      return reservation_active;

    case risk::OrderState::kFilled:
      return true;

    case risk::OrderState::kCancelled:
    case risk::OrderState::kRejected:
    case risk::OrderState::kAbortedBeforeSubmit:
      return !reservation_active;
  }

  return false;
}

[[nodiscard]] constexpr bool FitsSqliteInteger(std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max());
}

[[nodiscard]] bool IsValidEntry(const risk::OrderRegistryEntry& entry) noexcept {
  const auto required_notional = risk::ComputeOrderNotional(entry.identity.price, entry.identity.quantity);
  return required_notional && entry.reserved_notional >= *required_notional && FitsSqliteInteger(entry.client_order_id) &&
         FitsSqliteInteger(entry.market_id) && FitsSqliteInteger(entry.identity.strategy_id) &&
         entry.identity.client_order_id == entry.client_order_id && entry.identity.market_id == entry.market_id &&
         (!entry.venue_order_id || (!entry.venue_order_id->empty() && entry.venue_order_id->size() <= risk::kMaxVenueOrderIdBytes)) &&
         StateAndReservationAreValid(entry.state, entry.reservation_active);
}

[[nodiscard]] bool EventMatchesEntry(const risk::OrderRegistryEntry& entry, OrderAuditEventType type) noexcept {
  const auto resulting_state = ResultingStateFor(type);
  if (!resulting_state || *resulting_state != entry.state) {
    return false;
  }
  switch (type) {
    case OrderAuditEventType::kVenueFilled:
    case OrderAuditEventType::kReconciledFilled:
      return entry.reservation_active;
    case OrderAuditEventType::kExposureReconciled:
      return !entry.reservation_active;
    case OrderAuditEventType::kIntentPendingDurable:
    case OrderAuditEventType::kAbortedBeforeSubmit:
    case OrderAuditEventType::kVenueOpen:
    case OrderAuditEventType::kVenueRejected:
    case OrderAuditEventType::kVenueUnknown:
    case OrderAuditEventType::kReconciledOpen:
    case OrderAuditEventType::kReconciledCancelled:
    case OrderAuditEventType::kReconciledRejected:
      return true;
  }
  return false;
}

[[nodiscard]] bool ResetAndClear(sqlite3_stmt* statement) noexcept {
  // sqlite3_reset() resets the statement even when it returns the error
  // code from the previous sqlite3_step(). Only clear_bindings describes
  // the readiness of the newly reset statement.
  static_cast<void>(sqlite3_reset(statement));
  return sqlite3_clear_bindings(statement) == SQLITE_OK;
}

[[nodiscard]] bool BindEntry(sqlite3_stmt* statement, const risk::OrderRegistryEntry& entry) noexcept {
  const auto state = EncodeState(entry.state);
  const auto side = EncodeSide(entry.identity.side);

  if (!state || !side) {
    return false;
  }

  return sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(entry.client_order_id)) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 2, *state) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(entry.market_id)) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 4, entry.reserved_notional.micros) == SQLITE_OK &&
         sqlite3_bind_int(statement, 5, entry.reservation_active ? 1 : 0) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 6, static_cast<sqlite3_int64>(entry.identity.strategy_id)) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 7, *side) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 8, entry.identity.price.micros_per_unit) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 9, entry.identity.quantity.microunits) == SQLITE_OK &&
         (entry.venue_order_id ? sqlite3_bind_text(statement, 10, entry.venue_order_id->c_str(),
                                                   static_cast<int>(entry.venue_order_id->size()), SQLITE_TRANSIENT)
                               : sqlite3_bind_null(statement, 10)) == SQLITE_OK;
}

[[nodiscard]] bool BindEventIdentity(sqlite3_stmt* statement, const risk::OrderRegistryEntry& entry, OrderAuditEventType type) noexcept {
  const auto event_type = EncodeEventType(type);
  return event_type && sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(entry.client_order_id)) == SQLITE_OK &&
         sqlite3_bind_int64(statement, 2, *event_type) == SQLITE_OK;
}

[[nodiscard]] bool BindEvent(sqlite3_stmt* statement, const risk::OrderRegistryEntry& entry, OrderAuditEventType type) noexcept {
  const auto state = EncodeState(entry.state);
  return state && BindEventIdentity(statement, entry, type) && sqlite3_bind_int64(statement, 3, *state) == SQLITE_OK &&
         (entry.venue_order_id ? sqlite3_bind_text(statement, 4, entry.venue_order_id->c_str(),
                                                   static_cast<int>(entry.venue_order_id->size()), SQLITE_TRANSIENT)
                               : sqlite3_bind_null(statement, 4)) == SQLITE_OK;
}

[[nodiscard]] bool OptionalTextMatches(sqlite3_stmt* statement, int column, const std::optional<std::string>& expected) noexcept {
  if (!expected) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL;
  }
  return SqliteTextEquals(statement, column, *expected);
}

[[nodiscard]] bool ExistingEventMatches(sqlite3_stmt* statement, const risk::OrderRegistryEntry& entry) noexcept {
  const auto state = EncodeState(entry.state);
  const auto side = EncodeSide(entry.identity.side);
  if (!state || !side) {
    return false;
  }
  for (int column = 0; column < 9; ++column) {
    if (column != 1 && sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
      return false;
    }
  }
  return sqlite3_column_int64(statement, 0) == *state && OptionalTextMatches(statement, 1, entry.venue_order_id) &&
         sqlite3_column_int64(statement, 2) == static_cast<sqlite3_int64>(entry.client_order_id) &&
         sqlite3_column_int64(statement, 3) == static_cast<sqlite3_int64>(entry.market_id) &&
         sqlite3_column_int64(statement, 4) == entry.reserved_notional.micros &&
         sqlite3_column_int64(statement, 5) == static_cast<sqlite3_int64>(entry.identity.strategy_id) &&
         sqlite3_column_int64(statement, 6) == *side && sqlite3_column_int64(statement, 7) == entry.identity.price.micros_per_unit &&
         sqlite3_column_int64(statement, 8) == entry.identity.quantity.microunits && sqlite3_step(statement) == SQLITE_DONE;
}

[[nodiscard]] bool Rollback(sqlite3* database) noexcept {
  return Execute(database, "ROLLBACK;");
}

[[nodiscard]] bool RollbackAfterStatementFailure(sqlite3* database) noexcept {
  if (sqlite3_get_autocommit(database) != 0) {
    // SQLite already rolled the explicit transaction back.
    return true;
  }

  return Rollback(database) && sqlite3_get_autocommit(database) != 0;
}

[[nodiscard]] bool ReadEntry(sqlite3_stmt* statement, risk::OrderRegistryEntry& entry) noexcept {
  for (int column = 0; column < 9; ++column) {
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
      return false;
    }
  }

  const auto client_order_id = sqlite3_column_int64(statement, 0);
  const auto state_value = sqlite3_column_int64(statement, 1);
  const auto market_id = sqlite3_column_int64(statement, 2);
  const auto reserved_notional = sqlite3_column_int64(statement, 3);
  const auto reservation_active = sqlite3_column_int64(statement, 4);
  const auto strategy_id = sqlite3_column_int64(statement, 5);
  const auto side_value = sqlite3_column_int64(statement, 6);
  const auto price = sqlite3_column_int64(statement, 7);
  const auto quantity = sqlite3_column_int64(statement, 8);

  if (client_order_id < 0 || market_id < 0 || strategy_id < 0 || price < 0 || quantity < 0 || reserved_notional <= 0 ||
      (reservation_active != 0 && reservation_active != 1)) {
    return false;
  }

  const auto state = DecodeState(state_value);
  const auto active = reservation_active == 1;
  const auto side = DecodeSide(side_value);

  if (!state || !side || !StateAndReservationAreValid(*state, active)) {
    return false;
  }

  std::optional<std::string> venue_order_id;
  const auto venue_id_type = sqlite3_column_type(statement, 9);

  if (venue_id_type == SQLITE_TEXT) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 9));
    const auto size = sqlite3_column_bytes(statement, 9);
    if (text == nullptr || size <= 0 || static_cast<std::size_t>(size) > risk::kMaxVenueOrderIdBytes) {
      return false;
    }
    venue_order_id.emplace(text, static_cast<std::size_t>(size));
  } else if (venue_id_type != SQLITE_NULL) {
    return false;
  }

  entry = risk::OrderRegistryEntry{
      .client_order_id = static_cast<risk::ClientOrderId>(client_order_id),
      .state = *state,
      .market_id = static_cast<risk::MarketId>(market_id),
      .reserved_notional = risk::Money{.micros = reserved_notional},
      .reservation_active = active,
      .identity =
          risk::OrderIdentity{
              .strategy_id = static_cast<risk::StrategyId>(strategy_id),
              .client_order_id = static_cast<risk::ClientOrderId>(client_order_id),
              .market_id = static_cast<risk::MarketId>(market_id),
              .side = *side,
              .price = risk::Price{.micros_per_unit = price},
              .quantity = risk::Quantity{.microunits = quantity},
          },
      .venue_order_id = std::move(venue_order_id),
  };

  return true;
}

[[nodiscard]] bool ReadAuditEvent(sqlite3_stmt* statement, OrderAuditEvent& event) noexcept {
  for (int column = 0; column < 4; ++column) {
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
      return false;
    }
  }
  const auto sequence = sqlite3_column_int64(statement, 0);
  const auto client_order_id = sqlite3_column_int64(statement, 1);
  const auto type = DecodeEventType(sqlite3_column_int64(statement, 2));
  const auto state = DecodeState(sqlite3_column_int64(statement, 3));
  if (sequence <= 0 || client_order_id < 0 || !type || !state || ResultingStateFor(*type) != state) {
    return false;
  }
  std::optional<std::string> venue_order_id;
  const auto venue_type = sqlite3_column_type(statement, 4);
  if (venue_type == SQLITE_TEXT) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
    const auto size = sqlite3_column_bytes(statement, 4);
    if (text == nullptr || size <= 0 || static_cast<std::size_t>(size) > risk::kMaxVenueOrderIdBytes) {
      return false;
    }
    venue_order_id.emplace(text, static_cast<std::size_t>(size));
  } else if (venue_type != SQLITE_NULL) {
    return false;
  }
  event = OrderAuditEvent{
      .sequence = static_cast<std::uint64_t>(sequence),
      .client_order_id = static_cast<risk::ClientOrderId>(client_order_id),
      .type = *type,
      .resulting_state = *state,
      .venue_order_id = std::move(venue_order_id),
  };
  return true;
}

}  // namespace

std::expected<SqliteOrderStateStore, SqliteOrderStateStoreError> SqliteOrderStateStore::CreateNew(const std::filesystem::path& path,
                                                                                                  const StorageGeneration& generation) {
  if (!IsValidStorageGeneration(generation.value)) {
    return std::unexpected(SqliteOrderStateStoreError::kInvalidStorageGeneration);
  }
  return OpenWithMode(path, generation, true);
}

std::expected<SqliteOrderStateStore, SqliteOrderStateStoreError> SqliteOrderStateStore::OpenExisting(const std::filesystem::path& path) {
  return OpenWithMode(path, std::nullopt, false);
}

std::expected<SqliteOrderStateStore, SqliteOrderStateStoreError> SqliteOrderStateStore::OpenWithMode(
    const std::filesystem::path& path, std::optional<StorageGeneration> generation, bool create_new) {
  sqlite3* database{};
  const auto path_string = path.string();
  const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | (create_new ? SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE : 0);
  if (sqlite3_open_v2(path_string.c_str(), &database, flags, nullptr) != SQLITE_OK) {
    CloseResources(database, nullptr, nullptr);
    return std::unexpected(SqliteOrderStateStoreError::kOpenFailed);
  }

  static_cast<void>(sqlite3_extended_result_codes(database, 1));

  if (!Configure(database)) {
    CloseResources(database, nullptr, nullptr);
    return std::unexpected(SqliteOrderStateStoreError::kConfigurationFailed);
  }

  if (create_new) {
    if (!generation || !InitializeNewSchema(database, *generation)) {
      CloseResources(database, nullptr, nullptr);
      return std::unexpected(SqliteOrderStateStoreError::kSchemaInitializationFailed);
    }
  } else {
    generation = ValidateExistingSchema(database);
  }
  if (!generation) {
    CloseResources(database, nullptr, nullptr);
    return std::unexpected(SqliteOrderStateStoreError::kSchemaInitializationFailed);
  }

  sqlite3_stmt* upsert_statement{};
  sqlite3_stmt* load_statement{};

  if (sqlite3_prepare_v2(database, kUpsertSql, -1, &upsert_statement, nullptr) != SQLITE_OK ||
      sqlite3_prepare_v2(database, kLoadAllSql, -1, &load_statement, nullptr) != SQLITE_OK) {
    CloseResources(database, upsert_statement, load_statement);
    return std::unexpected(SqliteOrderStateStoreError::kSchemaInitializationFailed);
  }

  return SqliteOrderStateStore(database, upsert_statement, load_statement, std::move(*generation));
}

SqliteOrderStateStore::SqliteOrderStateStore(sqlite3* database, sqlite3_stmt* upsert_statement, sqlite3_stmt* load_statement,
                                             StorageGeneration generation) noexcept
    : database_(database), upsert_statement_(upsert_statement), load_statement_(load_statement), generation_(std::move(generation)) {}

SqliteOrderStateStore::~SqliteOrderStateStore() {
  Close();
}

SqliteOrderStateStore::SqliteOrderStateStore(SqliteOrderStateStore&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)),
      upsert_statement_(std::exchange(other.upsert_statement_, nullptr)),
      load_statement_(std::exchange(other.load_statement_, nullptr)),
      healthy_(std::exchange(other.healthy_, false)),
      checkpoint_pending_(std::exchange(other.checkpoint_pending_, false)),
      generation_(std::move(other.generation_)) {}

SqliteOrderStateStore& SqliteOrderStateStore::operator=(SqliteOrderStateStore&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  Close();

  database_ = std::exchange(other.database_, nullptr);
  upsert_statement_ = std::exchange(other.upsert_statement_, nullptr);
  load_statement_ = std::exchange(other.load_statement_, nullptr);
  healthy_ = std::exchange(other.healthy_, false);
  checkpoint_pending_ = std::exchange(other.checkpoint_pending_, false);
  generation_ = std::move(other.generation_);

  return *this;
}

bool SqliteOrderStateStore::PersistTransition(const risk::OrderRegistryEntry& entry, OrderAuditEventType event_type) noexcept {
  if (!healthy_ || database_ == nullptr || upsert_statement_ == nullptr || !IsValidEntry(entry) || !EventMatchesEntry(entry, event_type)) {
    return false;
  }

  // Keep checkpoint I/O outside the durability barrier of the current
  // write. A serious failure is therefore reported before any new
  // in-memory claim can become durable, while BUSY merely defers cleanup.
  if (checkpoint_pending_) {
    int wal_frames{};
    int checkpointed_frames{};
    const auto checkpoint_result =
        sqlite3_wal_checkpoint_v2(database_, nullptr, SQLITE_CHECKPOINT_PASSIVE, &wal_frames, &checkpointed_frames);
    const auto primary_checkpoint_result = checkpoint_result & 0xFF;

    if (primary_checkpoint_result == SQLITE_OK) {
      checkpoint_pending_ = wal_frames > checkpointed_frames;
    } else if (primary_checkpoint_result != SQLITE_BUSY && primary_checkpoint_result != SQLITE_LOCKED) {
      healthy_ = false;
      return false;
    }
  }

  if (!Execute(database_, "BEGIN IMMEDIATE;")) {
    if (sqlite3_get_autocommit(database_) == 0 && (!Rollback(database_) || sqlite3_get_autocommit(database_) == 0)) {
      std::terminate();
    }

    return false;
  }

  sqlite3_stmt* existing_event{};
  if (sqlite3_prepare_v2(database_, kFindEventSql, -1, &existing_event, nullptr) != SQLITE_OK ||
      !BindEventIdentity(existing_event, entry, event_type)) {
    static_cast<void>(sqlite3_finalize(existing_event));
    if (!RollbackAfterStatementFailure(database_)) {
      std::terminate();
    }
    return false;
  }
  const auto existing_step = sqlite3_step(existing_event);
  if (existing_step == SQLITE_ROW) {
    const auto matches = ExistingEventMatches(existing_event, entry);
    const auto finalized = sqlite3_finalize(existing_event) == SQLITE_OK;
    if (!RollbackAfterStatementFailure(database_)) {
      std::terminate();
    }
    return matches && finalized;
  }
  const auto lookup_finished = existing_step == SQLITE_DONE && sqlite3_finalize(existing_event) == SQLITE_OK;
  if (!lookup_finished || !ResetAndClear(upsert_statement_)) {
    if (!RollbackAfterStatementFailure(database_)) {
      std::terminate();
    }
    return false;
  }

  const auto statement_succeeded = BindEntry(upsert_statement_, entry) && sqlite3_step(upsert_statement_) == SQLITE_DONE;
  static_cast<void>(sqlite3_reset(upsert_statement_));

  if (!statement_succeeded) {
    if (!RollbackAfterStatementFailure(database_)) {
      // Returning false would authorize the coordinator to erase its RAM
      // identity even though this connection cannot prove rollback.
      std::terminate();
    }

    return false;
  }

  sqlite3_stmt* insert_event{};
  const auto event_succeeded = sqlite3_prepare_v2(database_, kInsertEventSql, -1, &insert_event, nullptr) == SQLITE_OK &&
                               BindEvent(insert_event, entry, event_type) && sqlite3_step(insert_event) == SQLITE_DONE;
  const auto event_finalized = sqlite3_finalize(insert_event) == SQLITE_OK;
  if (!event_succeeded || !event_finalized) {
    if (!RollbackAfterStatementFailure(database_)) {
      std::terminate();
    }
    return false;
  }

  if (!Execute(database_, "COMMIT;")) {
    if (sqlite3_get_autocommit(database_) != 0) {
      // With auto-checkpoint disabled, normal SQLite paths do not report a
      // post-commit error. If a custom VFS still produces one, committed
      // and auto-rolled-back outcomes are indistinguishable here.
      std::terminate();
    }

    if (!Rollback(database_) || sqlite3_get_autocommit(database_) == 0) {
      std::terminate();
    }

    return false;
  }

  checkpoint_pending_ = true;

  return true;
}

bool SqliteOrderStateStore::LoadAll(std::vector<risk::OrderRegistryEntry>& entries) noexcept {
  if (!healthy_ || load_statement_ == nullptr || !ResetAndClear(load_statement_)) {
    return false;
  }

  try {
    std::vector<risk::OrderRegistryEntry> loaded;
    std::optional<risk::ClientOrderId> previous_client_order_id;

    while (true) {
      const auto step_result = sqlite3_step(load_statement_);

      if (step_result == SQLITE_DONE) {
        break;
      }

      if (step_result != SQLITE_ROW) {
        static_cast<void>(sqlite3_reset(load_statement_));
        return false;
      }

      risk::OrderRegistryEntry entry;

      if (!ReadEntry(load_statement_, entry)) {
        static_cast<void>(sqlite3_reset(load_statement_));
        return false;
      }

      if (previous_client_order_id && entry.client_order_id <= *previous_client_order_id) {
        static_cast<void>(sqlite3_reset(load_statement_));
        return false;
      }

      loaded.push_back(entry);
      previous_client_order_id = entry.client_order_id;
    }

    if (sqlite3_reset(load_statement_) != SQLITE_OK) {
      return false;
    }

    entries.swap(loaded);
    return true;
  } catch (...) {
    static_cast<void>(sqlite3_reset(load_statement_));
    return false;
  }
}

bool SqliteOrderStateStore::LoadAuditEvents(std::vector<OrderAuditEvent>& events) noexcept {
  if (!healthy_ || database_ == nullptr) {
    return false;
  }
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(database_, kLoadAuditEventsSql, -1, &statement, nullptr) != SQLITE_OK) {
    return false;
  }
  try {
    std::vector<OrderAuditEvent> loaded;
    std::uint64_t previous_sequence{};
    while (true) {
      const auto step = sqlite3_step(statement);
      if (step == SQLITE_DONE) {
        break;
      }
      if (step != SQLITE_ROW) {
        static_cast<void>(sqlite3_finalize(statement));
        return false;
      }
      OrderAuditEvent event;
      if (!ReadAuditEvent(statement, event) || event.sequence <= previous_sequence) {
        static_cast<void>(sqlite3_finalize(statement));
        return false;
      }
      previous_sequence = event.sequence;
      loaded.push_back(std::move(event));
    }
    if (sqlite3_finalize(statement) != SQLITE_OK) {
      return false;
    }
    events.swap(loaded);
    return true;
  } catch (...) {
    static_cast<void>(sqlite3_finalize(statement));
    return false;
  }
}

void SqliteOrderStateStore::Close() noexcept {
  if (database_ == nullptr) {
    return;
  }

  if (sqlite3_get_autocommit(database_) == 0) {
    static_cast<void>(Rollback(database_));
  }

  CloseResources(database_, upsert_statement_, load_statement_);

  database_ = nullptr;
  upsert_statement_ = nullptr;
  load_statement_ = nullptr;
  healthy_ = false;
  checkpoint_pending_ = false;
}

}  // namespace botguard::execution
