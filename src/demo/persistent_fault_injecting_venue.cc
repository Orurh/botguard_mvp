#include "persistent_fault_injecting_venue.h"

#include <sqlite3.h>

#include <string>
#include <utility>

namespace botguard::demo {

std::optional<PersistentFaultInjectingVenue> PersistentFaultInjectingVenue::Open(const std::filesystem::path& path,
                                                                                 bool lose_submit_response) {
  sqlite3* database = nullptr;
  const auto path_string = path.string();
  if (sqlite3_open_v2(path_string.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) !=
      SQLITE_OK) {
    if (database != nullptr) {
      static_cast<void>(sqlite3_close(database));
    }
    return std::nullopt;
  }

  constexpr const char* schema = R"sql(
    PRAGMA journal_mode=WAL;
    PRAGMA synchronous=FULL;
    CREATE TABLE IF NOT EXISTS venue_stats (
      singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
      submit_count INTEGER NOT NULL
    );
    INSERT OR IGNORE INTO venue_stats(singleton, submit_count) VALUES(1, 0);
    CREATE TABLE IF NOT EXISTS venue_control (
      singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
      query_available INTEGER NOT NULL CHECK(query_available IN (0, 1))
    );
    INSERT OR IGNORE INTO venue_control(singleton, query_available) VALUES(1, 1);
    CREATE TABLE IF NOT EXISTS accepted_orders (
      client_order_id INTEGER PRIMARY KEY,
      strategy_id INTEGER NOT NULL,
      market_id INTEGER NOT NULL,
      side INTEGER NOT NULL,
      price INTEGER NOT NULL,
      quantity INTEGER NOT NULL
    );
  )sql";
  if (sqlite3_exec(database, schema, nullptr, nullptr, nullptr) != SQLITE_OK) {
    static_cast<void>(sqlite3_close(database));
    return std::nullopt;
  }
  return PersistentFaultInjectingVenue(database, lose_submit_response);
}

PersistentFaultInjectingVenue::~PersistentFaultInjectingVenue() {
  if (database_ != nullptr) {
    static_cast<void>(sqlite3_close(database_));
  }
}

PersistentFaultInjectingVenue::PersistentFaultInjectingVenue(PersistentFaultInjectingVenue&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)), lose_submit_response_(other.lose_submit_response_) {}

execution::VenuePreflightResult PersistentFaultInjectingVenue::Preflight(const risk::OrderIntent&) const noexcept {
  return execution::VenuePreflightResult::kSupported;
}

execution::VenueSubmitResult PersistentFaultInjectingVenue::Submit(const risk::OrderIntent& order) noexcept {
  sqlite3_stmt* statement = nullptr;
  constexpr const char* insert = R"sql(
    INSERT OR IGNORE INTO accepted_orders(
      client_order_id, strategy_id, market_id, side, price, quantity
    ) VALUES(?1, ?2, ?3, ?4, ?5, ?6);
  )sql";
  const auto began = sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
  const auto counted = began && sqlite3_exec(database_, "UPDATE venue_stats SET submit_count = submit_count + 1 WHERE singleton = 1;",
                                             nullptr, nullptr, nullptr) == SQLITE_OK;
  const auto prepared = counted && sqlite3_prepare_v2(database_, insert, -1, &statement, nullptr) == SQLITE_OK;
  const auto bound = prepared && sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(order.client_order_id)) == SQLITE_OK &&
                     sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(order.strategy_id)) == SQLITE_OK &&
                     sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(order.market_id)) == SQLITE_OK &&
                     sqlite3_bind_int(statement, 4, static_cast<int>(order.side)) == SQLITE_OK &&
                     sqlite3_bind_int64(statement, 5, order.price.micros_per_unit) == SQLITE_OK &&
                     sqlite3_bind_int64(statement, 6, order.quantity.microunits) == SQLITE_OK;
  const auto inserted = bound && sqlite3_step(statement) == SQLITE_DONE;
  if (statement != nullptr) {
    static_cast<void>(sqlite3_finalize(statement));
  }
  const auto committed = inserted && sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
  if (!committed) {
    static_cast<void>(sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr));
    return execution::VenueSubmitResult{.outcome = execution::SubmitOutcome::kUnknown, .venue_order_id = std::nullopt};
  }

  if (lose_submit_response_) {
    return execution::VenueSubmitResult{.outcome = execution::SubmitOutcome::kUnknown, .venue_order_id = std::nullopt};
  }
  return execution::VenueSubmitResult{
      .outcome = execution::SubmitOutcome::kOpen,
      .venue_order_id = "simulated-order-" + std::to_string(order.client_order_id),
  };
}

execution::VenueOrderQueryResult PersistentFaultInjectingVenue::Query(const execution::VenueOrderLookupKey& key) noexcept {
  sqlite3_stmt* availability = nullptr;
  const auto availability_ready = sqlite3_prepare_v2(database_, "SELECT query_available FROM venue_control WHERE singleton = 1;", -1,
                                                     &availability, nullptr) == SQLITE_OK;
  const auto available = availability_ready && sqlite3_step(availability) == SQLITE_ROW &&
                         sqlite3_column_type(availability, 0) == SQLITE_INTEGER && sqlite3_column_int(availability, 0) == 1 &&
                         sqlite3_step(availability) == SQLITE_DONE;
  if (availability != nullptr) {
    static_cast<void>(sqlite3_finalize(availability));
  }
  if (!available) {
    return std::unexpected(execution::VenueOrderQueryError::kUnavailable);
  }
  sqlite3_stmt* statement = nullptr;
  constexpr const char* select = R"sql(
    SELECT strategy_id, market_id, side, price, quantity
    FROM accepted_orders WHERE client_order_id = ?1;
  )sql";
  if (sqlite3_prepare_v2(database_, select, -1, &statement, nullptr) != SQLITE_OK ||
      sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(key.identity.client_order_id)) != SQLITE_OK) {
    if (statement != nullptr) {
      static_cast<void>(sqlite3_finalize(statement));
    }
    return std::unexpected(execution::VenueOrderQueryError::kUnavailable);
  }

  const auto step = sqlite3_step(statement);
  if (step == SQLITE_DONE) {
    static_cast<void>(sqlite3_finalize(statement));
    return std::optional<execution::VenueOrderObservation>{};
  }
  if (step != SQLITE_ROW) {
    static_cast<void>(sqlite3_finalize(statement));
    return std::unexpected(execution::VenueOrderQueryError::kUnavailable);
  }

  const auto matches = sqlite3_column_int64(statement, 0) == static_cast<sqlite3_int64>(key.identity.strategy_id) &&
                       sqlite3_column_int64(statement, 1) == static_cast<sqlite3_int64>(key.identity.market_id) &&
                       sqlite3_column_int(statement, 2) == static_cast<int>(key.identity.side) &&
                       sqlite3_column_int64(statement, 3) == key.identity.price.micros_per_unit &&
                       sqlite3_column_int64(statement, 4) == key.identity.quantity.microunits;
  static_cast<void>(sqlite3_finalize(statement));
  if (!matches) {
    return std::unexpected(execution::VenueOrderQueryError::kInvalidResponse);
  }
  return std::optional<execution::VenueOrderObservation>{execution::VenueOrderObservation{
      .state = execution::VenueOrderState::kOpen,
      .venue_order_id = "simulated-order-" + std::to_string(key.identity.client_order_id),
  }};
}

std::optional<std::uint64_t> PersistentFaultInjectingVenue::SubmitCount() const noexcept {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database_, "SELECT submit_count FROM venue_stats WHERE singleton = 1;", -1, &statement, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  const auto step = sqlite3_step(statement);
  const auto count =
      step == SQLITE_ROW ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0))} : std::nullopt;
  static_cast<void>(sqlite3_finalize(statement));
  return count;
}

bool PersistentFaultInjectingVenue::SetQueryAvailable(bool available) noexcept {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(database_, "UPDATE venue_control SET query_available = ?1 WHERE singleton = 1;", -1, &statement, nullptr) !=
      SQLITE_OK) {
    return false;
  }
  const auto updated = sqlite3_bind_int(statement, 1, available ? 1 : 0) == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE &&
                       sqlite3_changes(database_) == 1;
  return sqlite3_finalize(statement) == SQLITE_OK && updated;
}

void PersistentFaultInjectingVenue::SetLoseSubmitResponse(bool lose) noexcept {
  lose_submit_response_ = lose;
}

PersistentFaultInjectingVenue::PersistentFaultInjectingVenue(sqlite3* database, bool lose_submit_response) noexcept
    : database_(database), lose_submit_response_(lose_submit_response) {}

}  // namespace botguard::demo
