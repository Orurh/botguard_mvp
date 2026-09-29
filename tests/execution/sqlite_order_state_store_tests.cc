#include "execution/sqlite_order_state_store.h"

#include <sqlite3.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace execution = botguard::execution;
namespace risk = botguard::risk;

class TemporaryDatabase final {
 public:
  TemporaryDatabase() {
    static std::atomic_uint64_t sequence{};

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("botguard_sqlite_order_state_" + std::to_string(nonce) + "_" + std::to_string(sequence.fetch_add(1)) + ".db");
  }

  ~TemporaryDatabase() {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(path_, error));
    static_cast<void>(std::filesystem::remove(path_.string() + "-wal", error));
    static_cast<void>(std::filesystem::remove(path_.string() + "-shm", error));
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] execution::StorageGeneration TestGeneration() {
  return execution::StorageGeneration{.value = std::string(64, 'a')};
}

[[nodiscard]] std::expected<execution::SqliteOrderStateStore, execution::SqliteOrderStateStoreError> OpenStore(
    const std::filesystem::path& path) {
  return std::filesystem::exists(path) ? execution::SqliteOrderStateStore::OpenExisting(path)
                                       : execution::SqliteOrderStateStore::CreateNew(path, TestGeneration());
}

[[nodiscard]] risk::OrderRegistryEntry PendingEntry(risk::ClientOrderId client_order_id = 101, risk::MarketId market_id = 10,
                                                    std::int64_t reserved_notional = 25'000'000) {
  return risk::OrderRegistryEntry{
      .client_order_id = client_order_id,
      .state = risk::OrderState::kPendingSubmit,
      .market_id = market_id,
      .reserved_notional = risk::Money{.micros = reserved_notional},
      .reservation_active = true,
      .identity =
          risk::OrderIdentity{
              .strategy_id = 7,
              .client_order_id = client_order_id,
              .market_id = market_id,
              .side = risk::Side::kBuy,
              .price = risk::Price::FromCents(25),
              .quantity = risk::Quantity::FromWhole(1),
          },
  };
}

[[nodiscard]] execution::OrderAuditEventType EventTypeFor(const risk::OrderRegistryEntry& entry) noexcept {
  switch (entry.state) {
    case risk::OrderState::kPendingSubmit:
      return execution::OrderAuditEventType::kIntentPendingDurable;
    case risk::OrderState::kOpen:
      return execution::OrderAuditEventType::kVenueOpen;
    case risk::OrderState::kFilled:
      return entry.reservation_active ? execution::OrderAuditEventType::kVenueFilled : execution::OrderAuditEventType::kExposureReconciled;
    case risk::OrderState::kCancelled:
      return execution::OrderAuditEventType::kReconciledCancelled;
    case risk::OrderState::kRejected:
      return execution::OrderAuditEventType::kVenueRejected;
    case risk::OrderState::kAbortedBeforeSubmit:
      return execution::OrderAuditEventType::kAbortedBeforeSubmit;
    case risk::OrderState::kUnknown:
      return execution::OrderAuditEventType::kVenueUnknown;
  }
  return execution::OrderAuditEventType::kVenueUnknown;
}

[[nodiscard]] bool Persist(execution::SqliteOrderStateStore& store, const risk::OrderRegistryEntry& entry) noexcept {
  return store.PersistTransition(entry, EventTypeFor(entry));
}

void ExpectEntryEquals(const risk::OrderRegistryEntry& actual, const risk::OrderRegistryEntry& expected) {
  EXPECT_EQ(actual.client_order_id, expected.client_order_id);
  EXPECT_EQ(actual.state, expected.state);
  EXPECT_EQ(actual.market_id, expected.market_id);
  EXPECT_EQ(actual.reserved_notional, expected.reserved_notional);
  EXPECT_EQ(actual.reservation_active, expected.reservation_active);
  EXPECT_EQ(actual.identity, expected.identity);
  EXPECT_EQ(actual.venue_order_id, expected.venue_order_id);
}

using EntryMutator = void (*)(risk::OrderRegistryEntry&);

void ExpectExistingEventRejectsMismatchedIdentity(EntryMutator mutate) {
  TemporaryDatabase database;
  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto pending = PendingEntry();
  ASSERT_TRUE(opened->PersistTransition(pending, execution::OrderAuditEventType::kIntentPendingDurable));
  auto unknown = pending;
  unknown.state = risk::OrderState::kUnknown;
  ASSERT_TRUE(opened->PersistTransition(unknown, execution::OrderAuditEventType::kVenueUnknown));

  std::vector<execution::OrderAuditEvent> events_before;
  ASSERT_TRUE(opened->LoadAuditEvents(events_before));

  auto mismatched_pending = pending;
  mutate(mismatched_pending);
  EXPECT_FALSE(opened->PersistTransition(mismatched_pending, execution::OrderAuditEventType::kIntentPendingDurable));

  std::vector<risk::OrderRegistryEntry> entries;
  std::vector<execution::OrderAuditEvent> events_after;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_TRUE(opened->LoadAuditEvents(events_after));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), unknown);
  EXPECT_EQ(events_after, events_before);
}

TEST(SqliteOrderStateStoreTest, EmptyDatabaseLoadsEmptyState) {
  TemporaryDatabase database;

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  EXPECT_TRUE(entries.empty());

  std::vector<execution::OrderAuditEvent> events;
  ASSERT_TRUE(opened->LoadAuditEvents(events));
  EXPECT_TRUE(events.empty());
}

TEST(SqliteOrderStateStoreTest, PendingStateAndMatchingAuditEventCommitAtomically) {
  TemporaryDatabase database;
  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto pending = PendingEntry();
  ASSERT_TRUE(opened->PersistTransition(pending, execution::OrderAuditEventType::kIntentPendingDurable));

  std::vector<risk::OrderRegistryEntry> entries;
  std::vector<execution::OrderAuditEvent> events;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_TRUE(opened->LoadAuditEvents(events));
  ASSERT_EQ(entries.size(), 1U);
  ASSERT_EQ(events.size(), 1U);
  ExpectEntryEquals(entries.front(), pending);
  EXPECT_EQ(events.front().sequence, 1U);
  EXPECT_EQ(events.front().client_order_id, pending.client_order_id);
  EXPECT_EQ(events.front().type, execution::OrderAuditEventType::kIntentPendingDurable);
  EXPECT_EQ(events.front().resulting_state, risk::OrderState::kPendingSubmit);
  EXPECT_FALSE(events.front().venue_order_id.has_value());
}

TEST(SqliteOrderStateStoreTest, UpsertThenLoadReturnsEntry) {
  TemporaryDatabase database;

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto expected = PendingEntry();
  ASSERT_TRUE(Persist(*opened, expected));

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), expected);
}

TEST(SqliteOrderStateStoreTest, SecondUpsertReplacesSameClientOrderId) {
  TemporaryDatabase database;

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto pending = PendingEntry();
  auto open = pending;
  open.state = risk::OrderState::kOpen;
  open.reserved_notional = risk::Money{.micros = 30'000'000};

  ASSERT_TRUE(Persist(*opened, pending));
  ASSERT_TRUE(Persist(*opened, open));

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), open);
}

TEST(SqliteOrderStateStoreTest, MultipleClientOrderIdsAreLoaded) {
  TemporaryDatabase database;

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto first = PendingEntry(101, 10, 25'000'000);
  const auto second = PendingEntry(102, 20, 50'000'000);

  ASSERT_TRUE(Persist(*opened, first));
  ASSERT_TRUE(Persist(*opened, second));

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 2U);

  const auto find_entry = [&entries](risk::ClientOrderId client_order_id) {
    return std::ranges::find_if(entries, [client_order_id](const auto& entry) { return entry.client_order_id == client_order_id; });
  };

  const auto first_iterator = find_entry(first.client_order_id);
  const auto second_iterator = find_entry(second.client_order_id);

  ASSERT_NE(first_iterator, entries.end());
  ASSERT_NE(second_iterator, entries.end());
  ExpectEntryEquals(*first_iterator, first);
  ExpectEntryEquals(*second_iterator, second);
}

TEST(SqliteOrderStateStoreTest, ReplacementDoesNotCreateDuplicateRows) {
  TemporaryDatabase database;

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  auto entry = PendingEntry();

  ASSERT_TRUE(Persist(*opened, entry));

  entry.state = risk::OrderState::kUnknown;
  ASSERT_TRUE(Persist(*opened, entry));

  entry.state = risk::OrderState::kRejected;
  entry.reservation_active = false;
  ASSERT_TRUE(Persist(*opened, entry));

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), entry);
}

TEST(SqliteOrderStateStoreTest, FailedReplacementPreservesPreviousCommittedEntry) {
  TemporaryDatabase database;

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto committed = PendingEntry();
  ASSERT_TRUE(Persist(*opened, committed));

  auto invalid_replacement = committed;
  invalid_replacement.state = static_cast<risk::OrderState>(255);

  EXPECT_FALSE(Persist(*opened, invalid_replacement));

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), committed);
}

TEST(SqliteOrderStateStoreTest, BusyFailurePreservesPreviousCommittedEntry) {
  TemporaryDatabase database;

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto committed = PendingEntry();
  ASSERT_TRUE(Persist(*opened, committed));

  sqlite3* locking_connection{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &locking_connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(locking_connection, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);

  auto replacement = committed;
  replacement.state = risk::OrderState::kUnknown;

  EXPECT_FALSE(Persist(*opened, replacement));

  ASSERT_EQ(sqlite3_exec(locking_connection, "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(locking_connection), SQLITE_OK);

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), committed);

  std::vector<execution::OrderAuditEvent> events;
  ASSERT_TRUE(opened->LoadAuditEvents(events));
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front().type, execution::OrderAuditEventType::kIntentPendingDurable);
}

TEST(SqliteOrderStateStoreTest, FailedTransactionCreatesNeitherStateNorAuditEvent) {
  TemporaryDatabase database;
  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  sqlite3* locking_connection{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &locking_connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(locking_connection, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);

  EXPECT_FALSE(opened->PersistTransition(PendingEntry(), execution::OrderAuditEventType::kIntentPendingDurable));

  ASSERT_EQ(sqlite3_exec(locking_connection, "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(locking_connection), SQLITE_OK);

  std::vector<risk::OrderRegistryEntry> entries;
  std::vector<execution::OrderAuditEvent> events;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_TRUE(opened->LoadAuditEvents(events));
  EXPECT_TRUE(entries.empty());
  EXPECT_TRUE(events.empty());
}

TEST(SqliteOrderStateStoreTest, EventInsertFailureRollsBackStateChange) {
  TemporaryDatabase database;
  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  sqlite3* raw_database{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_database,
                         "CREATE TRIGGER reject_audit BEFORE INSERT ON order_event "
                         "BEGIN SELECT RAISE(ABORT, 'injected audit failure'); END;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_database), SQLITE_OK);

  EXPECT_FALSE(opened->PersistTransition(PendingEntry(), execution::OrderAuditEventType::kIntentPendingDurable));

  std::vector<risk::OrderRegistryEntry> entries;
  std::vector<execution::OrderAuditEvent> events;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_TRUE(opened->LoadAuditEvents(events));
  EXPECT_TRUE(entries.empty());
  EXPECT_TRUE(events.empty());
}

TEST(SqliteOrderStateStoreTest, RetriedSemanticTransitionIsIdempotentAndCannotRegressLaterState) {
  TemporaryDatabase database;
  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto pending = PendingEntry();
  ASSERT_TRUE(opened->PersistTransition(pending, execution::OrderAuditEventType::kIntentPendingDurable));
  ASSERT_TRUE(opened->PersistTransition(pending, execution::OrderAuditEventType::kIntentPendingDurable));

  auto unknown = pending;
  unknown.state = risk::OrderState::kUnknown;
  ASSERT_TRUE(opened->PersistTransition(unknown, execution::OrderAuditEventType::kVenueUnknown));
  ASSERT_TRUE(opened->PersistTransition(pending, execution::OrderAuditEventType::kIntentPendingDurable));

  std::vector<risk::OrderRegistryEntry> entries;
  std::vector<execution::OrderAuditEvent> events;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_TRUE(opened->LoadAuditEvents(events));
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries.front().state, risk::OrderState::kUnknown);
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].type, execution::OrderAuditEventType::kIntentPendingDurable);
  EXPECT_EQ(events[1].type, execution::OrderAuditEventType::kVenueUnknown);
  EXPECT_LT(events[0].sequence, events[1].sequence);
}

TEST(SqliteOrderStateStoreTest, ExistingSemanticEventWithDifferentStrategyIdFails) {
  ExpectExistingEventRejectsMismatchedIdentity(
      [](risk::OrderRegistryEntry& entry) { entry.identity.strategy_id = entry.identity.strategy_id + 1; });
}

TEST(SqliteOrderStateStoreTest, ExistingSemanticEventWithDifferentPriceOrQuantityFails) {
  ExpectExistingEventRejectsMismatchedIdentity([](risk::OrderRegistryEntry& entry) { entry.identity.price = risk::Price::FromCents(26); });
  ExpectExistingEventRejectsMismatchedIdentity(
      [](risk::OrderRegistryEntry& entry) { entry.identity.quantity = risk::Quantity::FromWhole(2); });
}

TEST(SqliteOrderStateStoreTest, ExistingSemanticEventWithDifferentReservedNotionalFails) {
  ExpectExistingEventRejectsMismatchedIdentity(
      [](risk::OrderRegistryEntry& entry) { entry.reserved_notional = risk::Money{.micros = entry.reserved_notional.micros - 1}; });
}

TEST(SqliteOrderStateStoreTest, FilledAuditEventsRequireTheMatchingReservationPhase) {
  TemporaryDatabase database;
  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto pending = PendingEntry();
  ASSERT_TRUE(opened->PersistTransition(pending, execution::OrderAuditEventType::kIntentPendingDurable));
  auto filled_reserved = pending;
  filled_reserved.state = risk::OrderState::kFilled;
  auto filled_released = filled_reserved;
  filled_released.reservation_active = false;

  EXPECT_FALSE(opened->PersistTransition(filled_released, execution::OrderAuditEventType::kVenueFilled));
  EXPECT_FALSE(opened->PersistTransition(filled_released, execution::OrderAuditEventType::kReconciledFilled));
  EXPECT_FALSE(opened->PersistTransition(filled_reserved, execution::OrderAuditEventType::kExposureReconciled));

  ASSERT_TRUE(opened->PersistTransition(filled_reserved, execution::OrderAuditEventType::kVenueFilled));
  ASSERT_TRUE(opened->PersistTransition(filled_released, execution::OrderAuditEventType::kExposureReconciled));

  std::vector<risk::OrderRegistryEntry> entries;
  std::vector<execution::OrderAuditEvent> events;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_TRUE(opened->LoadAuditEvents(events));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), filled_released);
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].type, execution::OrderAuditEventType::kIntentPendingDurable);
  EXPECT_EQ(events[1].type, execution::OrderAuditEventType::kVenueFilled);
  EXPECT_EQ(events[2].type, execution::OrderAuditEventType::kExposureReconciled);
}

TEST(SqliteOrderStateStoreTest, StateSurvivesCloseAndReopen) {
  TemporaryDatabase database;
  const auto expected = PendingEntry();

  {
    auto opened = OpenStore(database.Path());
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(Persist(*opened, expected));
  }

  auto reopened = OpenStore(database.Path());
  ASSERT_TRUE(reopened.has_value());

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(reopened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), expected);
}

#if defined(__unix__) || defined(__APPLE__)
TEST(SqliteOrderStateStoreTest, StateSurvivesProcessExitWithoutDestructors) {
  TemporaryDatabase database;
  const auto expected = PendingEntry();

  const auto child = fork();
  ASSERT_GE(child, 0);

  if (child == 0) {
    auto opened = OpenStore(database.Path());

    if (!opened) {
      _exit(10);
    }

    // Hold a read snapshot so the store's post-commit passive checkpoint
    // cannot move the new frame out of WAL. The parent must therefore
    // recover the committed state from WAL after this process exits.
    sqlite3* reader{};
    const auto path = database.Path().string();

    if (sqlite3_open_v2(path.c_str(), &reader, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
      _exit(11);
    }

    if (sqlite3_exec(reader, "BEGIN; SELECT COUNT(*) FROM order_state;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      _exit(12);
    }

    if (!Persist(*opened, expected)) {
      _exit(13);
    }

    // Deliberately bypass all C++ and SQLite destructors.
    _exit(0);
  }

  int status{};
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  auto reopened = OpenStore(database.Path());
  ASSERT_TRUE(reopened.has_value());

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(reopened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), expected);
}
#endif

TEST(SqliteOrderStateStoreTest, ReservationActiveSurvivesCloseAndReopen) {
  TemporaryDatabase database;
  auto expected = PendingEntry();
  expected.state = risk::OrderState::kOpen;

  {
    auto opened = OpenStore(database.Path());
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(Persist(*opened, expected));
  }

  auto reopened = OpenStore(database.Path());
  ASSERT_TRUE(reopened.has_value());

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(reopened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_TRUE(entries.front().reservation_active);
  ExpectEntryEquals(entries.front(), expected);
}

TEST(SqliteOrderStateStoreTest, FilledWithReleasedReservationSurvivesReopen) {
  TemporaryDatabase database;
  auto expected = PendingEntry();
  expected.state = risk::OrderState::kFilled;
  expected.reservation_active = false;

  {
    auto opened = OpenStore(database.Path());
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(Persist(*opened, expected));
  }

  auto reopened = OpenStore(database.Path());
  ASSERT_TRUE(reopened.has_value());

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(reopened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), expected);
}

TEST(SqliteOrderStateStoreTest, UnknownSurvivesCloseAndReopen) {
  TemporaryDatabase database;
  auto expected = PendingEntry();
  expected.state = risk::OrderState::kUnknown;

  {
    auto opened = OpenStore(database.Path());
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(Persist(*opened, expected));
  }

  auto reopened = OpenStore(database.Path());
  ASSERT_TRUE(reopened.has_value());

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(reopened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), expected);
}

TEST(SqliteOrderStateStoreTest, OriginalIdentityAndVenueAcknowledgementSurviveReopen) {
  TemporaryDatabase database;
  auto expected = PendingEntry();
  expected.state = risk::OrderState::kOpen;
  expected.identity.strategy_id = 99;
  expected.identity.side = risk::Side::kSell;
  expected.identity.price = risk::Price::FromCents(73);
  expected.identity.quantity = risk::Quantity{.microunits = 1'250'000};
  expected.venue_order_id = "prediction-market-order/abc-123";

  {
    auto opened = OpenStore(database.Path());
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(Persist(*opened, expected));
  }

  auto reopened = OpenStore(database.Path());
  ASSERT_TRUE(reopened.has_value());

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(reopened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), expected);
}

TEST(SqliteOrderStateStoreTest, LargeInt64ValuesRoundTripExactly) {
  TemporaryDatabase database;
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto expected = PendingEntry(static_cast<risk::ClientOrderId>(maximum), static_cast<risk::MarketId>(maximum), maximum);

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());
  ASSERT_TRUE(Persist(*opened, expected));

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  ASSERT_EQ(entries.size(), 1U);
  ExpectEntryEquals(entries.front(), expected);
}

TEST(SqliteOrderStateStoreTest, UnsignedIdsOutsideSqliteIntegerRangeAreRejected) {
  TemporaryDatabase database;

  auto opened = OpenStore(database.Path());
  ASSERT_TRUE(opened.has_value());

  const auto too_large = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;

  EXPECT_FALSE(Persist(*opened, PendingEntry(too_large)));
  EXPECT_FALSE(Persist(*opened, PendingEntry(101, too_large)));

  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(opened->LoadAll(entries));
  EXPECT_TRUE(entries.empty());
}

TEST(SqliteOrderStateStoreTest, CorruptStateCausesLoadFailure) {
  TemporaryDatabase database;
  const auto committed = PendingEntry();

  {
    auto opened = OpenStore(database.Path());
    ASSERT_TRUE(opened.has_value());
    ASSERT_TRUE(Persist(*opened, committed));
  }

  sqlite3* raw_database{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_database, "UPDATE order_state SET state = 255 WHERE client_order_id = 101;", nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_database), SQLITE_OK);

  auto reopened = OpenStore(database.Path());
  ASSERT_TRUE(reopened.has_value());

  std::vector<risk::OrderRegistryEntry> entries{PendingEntry(999)};
  EXPECT_FALSE(reopened->LoadAll(entries));

  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries.front().client_order_id, 999U);
}

TEST(SqliteOrderStateStoreTest, IncompatibleExistingSchemaIsRejected) {
  TemporaryDatabase database;

  sqlite3* raw_database{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_database, "CREATE TABLE order_state (client_order_id TEXT PRIMARY KEY);", nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_database), SQLITE_OK);

  const auto opened = OpenStore(database.Path());
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error(), execution::SqliteOrderStateStoreError::kSchemaInitializationFailed);
}

TEST(SqliteOrderStateStoreTest, VersionedDatabaseWithoutOrderStateTableIsRejected) {
  TemporaryDatabase database;

  sqlite3* raw_database{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_database,
                         "CREATE TABLE storage_metadata ("
                         "singleton INTEGER PRIMARY KEY CHECK (singleton = 1), generation TEXT NOT NULL);"
                         "INSERT INTO storage_metadata(singleton, generation) VALUES(1, "
                         "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa');"
                         "PRAGMA user_version = 3;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_database), SQLITE_OK);

  const auto opened = OpenStore(database.Path());
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error(), execution::SqliteOrderStateStoreError::kSchemaInitializationFailed);
}

TEST(SqliteOrderStateStoreTest, LegacyVersionOneSchemaIsRejectedFailClosed) {
  TemporaryDatabase database;

  sqlite3* raw_database{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_database,
                         "CREATE TABLE order_state ("
                         "client_order_id INTEGER PRIMARY KEY, state INTEGER NOT NULL, market_id INTEGER NOT NULL, "
                         "reserved_notional INTEGER NOT NULL, reservation_active INTEGER NOT NULL);"
                         "PRAGMA user_version = 1;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_database), SQLITE_OK);

  const auto opened = OpenStore(database.Path());
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error(), execution::SqliteOrderStateStoreError::kSchemaInitializationFailed);
}

TEST(SqliteOrderStateStoreTest, ExistingOrderStateTriggerIsRejected) {
  TemporaryDatabase database;

  {
    auto opened = OpenStore(database.Path());
    ASSERT_TRUE(opened.has_value());
  }

  sqlite3* raw_database{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_database,
                         "CREATE TRIGGER erase_inserted_order AFTER INSERT ON order_state "
                         "BEGIN DELETE FROM order_state WHERE client_order_id = NEW.client_order_id; END;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_database), SQLITE_OK);

  const auto opened = OpenStore(database.Path());
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error(), execution::SqliteOrderStateStoreError::kSchemaInitializationFailed);
}

TEST(SqliteOrderStateStoreTest, ExistingOrderEventTriggerIsRejected) {
  TemporaryDatabase database;

  {
    auto opened = OpenStore(database.Path());
    ASSERT_TRUE(opened.has_value());
  }

  sqlite3* raw_database{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_database,
                         "CREATE TRIGGER mutate_audit AFTER INSERT ON order_event "
                         "BEGIN UPDATE order_event SET venue_order_id = 'mutated' WHERE sequence = NEW.sequence; END;",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_database), SQLITE_OK);

  const auto opened = OpenStore(database.Path());
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error(), execution::SqliteOrderStateStoreError::kSchemaInitializationFailed);
}

}  // namespace
