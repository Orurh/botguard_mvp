#include "execution/order_submission_coordinator.h"
#include "execution/sqlite_order_state_store.h"
#include "execution/venue_order_query.h"
#include "execution/venue_startup_reconciler.h"

#include <sqlite3.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace {

using namespace std::chrono_literals;

namespace execution = botguard::execution;
namespace risk = botguard::risk;

class TemporaryDatabase final {
 public:
  TemporaryDatabase() {
    static std::atomic_uint64_t sequence{};

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("botguard_sqlite_submission_recovery_" + std::to_string(nonce) + "_" + std::to_string(sequence.fetch_add(1)) + ".db");
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

class SqliteWriteLock final {
 public:
  explicit SqliteWriteLock(const std::filesystem::path& path) {
    const auto path_string = path.string();
    open_succeeded_ = sqlite3_open_v2(path_string.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK;

    if (open_succeeded_) {
      open_succeeded_ = sqlite3_busy_timeout(database_, 0) == SQLITE_OK;
    }
  }

  ~SqliteWriteLock() {
    Release();

    if (database_ != nullptr) {
      static_cast<void>(sqlite3_close(database_));
    }
  }

  SqliteWriteLock(const SqliteWriteLock&) = delete;
  SqliteWriteLock& operator=(const SqliteWriteLock&) = delete;

  [[nodiscard]] bool Acquire() noexcept {
    if (!open_succeeded_ || acquired_) {
      return false;
    }

    acquired_ = sqlite3_exec(database_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
    return acquired_;
  }

  void Release() noexcept {
    if (!acquired_) {
      return;
    }

    static_cast<void>(sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr));
    acquired_ = false;
  }

  [[nodiscard]] bool Acquired() const noexcept { return acquired_; }

 private:
  sqlite3* database_{};
  bool open_succeeded_{};
  bool acquired_{};
};

class FixedAccountStateProvider final : public execution::AuthoritativeAccountStateProvider {
 public:
  explicit FixedAccountStateProvider(risk::MonotonicClock::time_point received_at) noexcept
      : cursor_{
            .version = execution::AccountStateVersion{.value = 1},
            .received_at = received_at,
        },
        snapshot_{
            .state =
                risk::AccountState{
                    .market_gross_exposure = risk::Money::FromWholeUsd(100),
                    .total_gross_exposure = risk::Money::FromWholeUsd(500),
                    .pnl_since_baseline = risk::Money::FromWholeUsd(10),
                },
            .version = execution::AccountStateVersion{.value = 1},
            .received_at = received_at,
        } {}

  [[nodiscard]] std::optional<execution::AccountStateCursor> CurrentCursor() const noexcept override { return cursor_; }

  [[nodiscard]] std::optional<execution::AccountStateSnapshot> Snapshot(risk::MarketId /*market_id*/) const noexcept override {
    return snapshot_;
  }

 private:
  execution::AccountStateCursor cursor_;
  execution::AccountStateSnapshot snapshot_;
};

class FixedMarketDataFreshnessProvider final : public execution::MarketDataFreshnessProvider {
 public:
  void Set(risk::MonotonicClock::time_point received_at) noexcept {
    cursor_ = execution::MarketDataCursor{.market_id = 10, .version = ++version_, .received_at = received_at};
  }

  [[nodiscard]] std::optional<execution::MarketDataCursor> Current(risk::MarketId /*market_id*/) const noexcept override { return cursor_; }

 private:
  std::uint64_t version_{};
  execution::MarketDataCursor cursor_{};
};

[[nodiscard]] execution::MarketDataFreshnessProvider& MarketDataAt(risk::MonotonicClock::time_point received_at) noexcept {
  static thread_local FixedMarketDataFreshnessProvider provider;
  provider.Set(received_at);
  return provider;
}

class FixedTradingDayProvider final : public execution::TradingDayProvider {
 public:
  [[nodiscard]] std::optional<std::int64_t> CurrentUtcDay() noexcept override { return 20'000; }
};

[[nodiscard]] execution::TradingDayProvider& TradingDay() noexcept {
  static thread_local FixedTradingDayProvider provider;
  return provider;
}

[[nodiscard]] execution::TradingDayContext TradingContext() noexcept {
  return execution::TradingDayContext{.utc_day = 20'000, .baseline_equity = risk::Money::FromWholeUsd(1'000)};
}

class FixedOutcomeSubmitter final : public execution::OrderSubmitter {
 public:
  explicit FixedOutcomeSubmitter(execution::SubmitOutcome outcome, std::optional<std::string> venue_order_id = std::nullopt,
                                 execution::VenuePreflightResult preflight = execution::VenuePreflightResult::kSupported) noexcept
      : outcome_(outcome), venue_order_id_(std::move(venue_order_id)), preflight_(preflight) {}

  [[nodiscard]] execution::VenuePreflightResult Preflight(const risk::OrderIntent&) const noexcept override { return preflight_; }

  [[nodiscard]] execution::VenueSubmitResult Submit(const risk::OrderIntent& /*order*/) noexcept override {
    ++submit_count_;
    return execution::VenueSubmitResult{.outcome = outcome_, .venue_order_id = venue_order_id_};
  }

  [[nodiscard]] std::size_t SubmitCount() const noexcept { return submit_count_; }

 private:
  execution::SubmitOutcome outcome_;
  std::optional<std::string> venue_order_id_;
  execution::VenuePreflightResult preflight_;
  std::size_t submit_count_{};
};

class LockingSubmitter final : public execution::OrderSubmitter {
 public:
  explicit LockingSubmitter(SqliteWriteLock& lock) noexcept : lock_(lock) {}

  [[nodiscard]] execution::VenuePreflightResult Preflight(const risk::OrderIntent&) const noexcept override {
    return execution::VenuePreflightResult::kSupported;
  }

  [[nodiscard]] execution::VenueSubmitResult Submit(const risk::OrderIntent& /*order*/) noexcept override {
    ++submit_count_;
    lock_acquired_during_submit_ = lock_.get().Acquire();
    return execution::VenueSubmitResult{.outcome = execution::SubmitOutcome::kOpen};
  }

  [[nodiscard]] std::size_t SubmitCount() const noexcept { return submit_count_; }
  [[nodiscard]] bool LockAcquiredDuringSubmit() const noexcept { return lock_acquired_during_submit_; }

 private:
  std::reference_wrapper<SqliteWriteLock> lock_;
  std::size_t submit_count_{};
  bool lock_acquired_during_submit_{};
};

enum class AmbiguousSubmitPoint : std::uint8_t {
  kBeforeAccept,
  kAfterAccept,
};

class StatefulFakeVenue final : public execution::OrderSubmitter, public execution::VenueOrderQuery {
 public:
  explicit StatefulFakeVenue(AmbiguousSubmitPoint submit_point) noexcept : submit_point_(submit_point) {}

  [[nodiscard]] execution::VenuePreflightResult Preflight(const risk::OrderIntent&) const noexcept override {
    return execution::VenuePreflightResult::kSupported;
  }

  [[nodiscard]] execution::VenueSubmitResult Submit(const risk::OrderIntent& order) noexcept override {
    ++submit_count_;

    if (submit_point_ == AmbiguousSubmitPoint::kAfterAccept) {
      accepted_orders_.insert_or_assign(order.client_order_id, order);
    }

    return execution::VenueSubmitResult{.outcome = execution::SubmitOutcome::kUnknown};
  }

  [[nodiscard]] execution::VenueOrderQueryResult Query(const execution::VenueOrderLookupKey& key) noexcept override {
    ++query_count_;

    if (!query_available_) {
      return std::unexpected(execution::VenueOrderQueryError::kUnavailable);
    }

    const auto accepted = accepted_orders_.find(key.identity.client_order_id);

    if (accepted == accepted_orders_.end()) {
      return std::optional<execution::VenueOrderObservation>{};
    }

    const auto& original = accepted->second;
    const auto identity_matches = key.identity.strategy_id == original.strategy_id &&
                                  key.identity.client_order_id == original.client_order_id &&
                                  key.identity.market_id == original.market_id && key.identity.side == original.side &&
                                  key.identity.price == original.price && key.identity.quantity == original.quantity;

    if (!identity_matches) {
      return std::unexpected(execution::VenueOrderQueryError::kInvalidResponse);
    }

    return std::optional<execution::VenueOrderObservation>{execution::VenueOrderObservation{
        .state = execution::VenueOrderState::kOpen,
        .venue_order_id = "fake-venue-order-" + std::to_string(key.identity.client_order_id),
    }};
  }

  void SetQueryAvailable(bool available) noexcept { query_available_ = available; }

  [[nodiscard]] bool Contains(risk::ClientOrderId client_order_id) const { return accepted_orders_.contains(client_order_id); }
  [[nodiscard]] std::size_t SubmitCount() const noexcept { return submit_count_; }
  [[nodiscard]] std::size_t QueryCount() const noexcept { return query_count_; }

 private:
  AmbiguousSubmitPoint submit_point_;
  bool query_available_{true};
  std::unordered_map<risk::ClientOrderId, risk::OrderIntent> accepted_orders_;
  std::size_t submit_count_{};
  std::size_t query_count_{};
};

class NoOpStartupReconciler final : public execution::StartupReconciler {
 public:
  [[nodiscard]] bool Reconcile(execution::ReconciliationCoordinator& /*reconciliation*/) noexcept override { return true; }
};

class RejectUnresolvedStartupReconciler final : public execution::StartupReconciler {
 public:
  [[nodiscard]] bool Reconcile(execution::ReconciliationCoordinator& reconciliation) noexcept override {
    for (const auto& entry : reconciliation.Snapshot()) {
      if (entry.state != risk::OrderState::kPendingSubmit && entry.state != risk::OrderState::kUnknown) {
        continue;
      }

      if (reconciliation.MarkRejected(entry.client_order_id) != execution::ReconciliationUpdateStatus::kApplied) {
        return false;
      }
    }

    return true;
  }
};

class OpenUnresolvedStartupReconciler final : public execution::StartupReconciler {
 public:
  [[nodiscard]] bool Reconcile(execution::ReconciliationCoordinator& reconciliation) noexcept override {
    for (const auto& entry : reconciliation.Snapshot()) {
      if (entry.state != risk::OrderState::kPendingSubmit && entry.state != risk::OrderState::kUnknown) {
        continue;
      }

      if (reconciliation.MarkOpen(entry.client_order_id) != execution::ReconciliationUpdateStatus::kApplied) {
        return false;
      }
    }

    return true;
  }
};

[[nodiscard]] risk::RiskLimits DefaultLimits() {
  return risk::RiskLimits{
      .max_order_notional = risk::Money::FromWholeUsd(100),
      .max_market_gross_exposure = risk::Money::FromWholeUsd(500),
      .max_total_gross_exposure = risk::Money::FromWholeUsd(2'000),
      .max_loss_since_baseline = risk::Money::FromWholeUsd(250),
      .max_market_data_age = 1s,
  };
}

[[nodiscard]] risk::OrderIntent ValidOrder(risk::MonotonicClock::time_point now) {
  return risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(50),
      .quantity = risk::Quantity::FromWhole(100),
      .market_data_received_at = now,
  };
}

[[nodiscard]] execution::AccountStateSafetyPolicy DefaultAccountStatePolicy() {
  return execution::AccountStateSafetyPolicy{.max_age = 1s};
}

[[nodiscard]] auto Recover(risk::OrderRegistry& registry, execution::OrderStateStore& store, execution::StartupReconciler& reconciler,
                           execution::AuthoritativeAccountStateProvider& account_state_provider, risk::MonotonicClock::time_point now) {
  return execution::StartupRecoverySession::Recover(registry, store, reconciler, account_state_provider, DefaultAccountStatePolicy(), now);
}

TEST(SqliteSubmissionRecoveryTest, OpenOrderSurvivesRestartAndDuplicateIntentIsNeverResubmitted) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();
  const auto order = ValidOrder(now);

  {
    auto store = OpenStore(database.Path());
    ASSERT_TRUE(store.has_value());

    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;
    FixedOutcomeSubmitter submitter(execution::SubmitOutcome::kOpen, "venue-open-101");

    auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    const auto result = coordinator.ExecuteAtForTesting(order, now);

    ASSERT_EQ(result.status, execution::SubmissionStatus::kSubmitted);
    ASSERT_EQ(result.submit_outcome, execution::SubmitOutcome::kOpen);
    EXPECT_EQ(submitter.SubmitCount(), 1U);
    EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kOpen);
  }

  // All runtime objects and the first SQLite connection are gone.
  // Reopening the database models a new process lifetime.
  auto reopened_store = OpenStore(database.Path());
  ASSERT_TRUE(reopened_store.has_value());

  risk::RiskEngine restarted_engine(DefaultLimits());
  risk::OrderRegistry restored_registry;
  FixedAccountStateProvider restarted_account_state_provider(now);
  NoOpStartupReconciler restarted_reconciler;
  FixedOutcomeSubmitter restarted_submitter(execution::SubmitOutcome::kOpen);

  auto restarted_recovery = Recover(restored_registry, *reopened_store, restarted_reconciler, restarted_account_state_provider, now);
  ASSERT_TRUE(restarted_recovery.has_value());
  ASSERT_EQ(restored_registry.State(order.client_order_id), risk::OrderState::kOpen);
  EXPECT_EQ(restored_registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
  const auto restored_record = restored_registry.Record(order.client_order_id);
  ASSERT_TRUE(restored_record.has_value());
  EXPECT_EQ(restored_record->identity.strategy_id, order.strategy_id);
  EXPECT_EQ(restored_record->identity.client_order_id, order.client_order_id);
  EXPECT_EQ(restored_record->identity.market_id, order.market_id);
  EXPECT_EQ(restored_record->identity.side, order.side);
  EXPECT_EQ(restored_record->identity.price, order.price);
  EXPECT_EQ(restored_record->identity.quantity, order.quantity);
  EXPECT_EQ(restored_record->venue_order_id, "venue-open-101");

  execution::OrderSubmissionCoordinator restarted_coordinator(restarted_engine, std::move(*restarted_recovery), restarted_submitter,
                                                              MarketDataAt(now), TradingDay(), TradingContext());
  const auto retry = restarted_coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(retry.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_TRUE(retry.risk_decision.HasReason(risk::RejectReason::kDuplicateClientOrderId));
  EXPECT_EQ(restarted_submitter.SubmitCount(), 0U);
}

TEST(SqliteSubmissionRecoveryTest, UnsupportedVenueIntentCreatesNoDurableOrderState) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();

  {
    auto store = OpenStore(database.Path());
    ASSERT_TRUE(store.has_value());
    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;
    FixedOutcomeSubmitter submitter(execution::SubmitOutcome::kOpen, std::nullopt, execution::VenuePreflightResult::kUnsupported);
    auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    const auto result = coordinator.ExecuteAtForTesting(ValidOrder(now), now);

    EXPECT_EQ(result.status, execution::SubmissionStatus::kVenuePreflightRejected);
    EXPECT_EQ(submitter.SubmitCount(), 0U);
    EXPECT_TRUE(registry.Snapshot().empty());
  }

  auto reopened = execution::SqliteOrderStateStore::OpenExisting(database.Path());
  ASSERT_TRUE(reopened.has_value());
  std::vector<risk::OrderRegistryEntry> entries;
  ASSERT_TRUE(reopened->LoadAll(entries));
  EXPECT_TRUE(entries.empty());
}

TEST(SqliteSubmissionRecoveryTest, UnknownOrderRequiresDurableReconciliationBeforeTradingCanResume) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();
  const auto order = ValidOrder(now);

  {
    auto store = OpenStore(database.Path());
    ASSERT_TRUE(store.has_value());

    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;
    FixedOutcomeSubmitter submitter(execution::SubmitOutcome::kUnknown);

    auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    const auto result = coordinator.ExecuteAtForTesting(order, now);

    ASSERT_EQ(result.status, execution::SubmissionStatus::kSubmitted);
    ASSERT_EQ(result.submit_outcome, execution::SubmitOutcome::kUnknown);
    EXPECT_EQ(submitter.SubmitCount(), 1U);
    EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);
  }

  {
    auto reopened_store = OpenStore(database.Path());
    ASSERT_TRUE(reopened_store.has_value());

    risk::OrderRegistry unresolved_registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;

    auto recovery = Recover(unresolved_registry, *reopened_store, reconciler, account_state_provider, now);

    ASSERT_FALSE(recovery.has_value());
    EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kUnresolvedOrdersRemain);
    EXPECT_EQ(unresolved_registry.State(order.client_order_id), risk::OrderState::kUnknown);
    EXPECT_EQ(unresolved_registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
  }

  {
    auto reopened_store = OpenStore(database.Path());
    ASSERT_TRUE(reopened_store.has_value());

    risk::RiskEngine restarted_engine(DefaultLimits());
    risk::OrderRegistry reconciled_registry;
    FixedAccountStateProvider account_state_provider(now);
    RejectUnresolvedStartupReconciler reconciler;
    FixedOutcomeSubmitter restarted_submitter(execution::SubmitOutcome::kOpen);

    auto recovery = Recover(reconciled_registry, *reopened_store, reconciler, account_state_provider, now);
    ASSERT_TRUE(recovery.has_value());
    ASSERT_EQ(reconciled_registry.State(order.client_order_id), risk::OrderState::kRejected);
    EXPECT_EQ(reconciled_registry.TotalReservedExposure(), risk::Money{});

    execution::OrderSubmissionCoordinator coordinator(restarted_engine, std::move(*recovery), restarted_submitter, MarketDataAt(now),
                                                      TradingDay(), TradingContext());
    const auto retry = coordinator.ExecuteAtForTesting(order, now);

    EXPECT_EQ(retry.status, execution::SubmissionStatus::kRiskRejected);
    EXPECT_TRUE(retry.risk_decision.HasReason(risk::RejectReason::kDuplicateClientOrderId));
    EXPECT_EQ(restarted_submitter.SubmitCount(), 0U);
  }

  // Reconciliation itself must have replaced UNKNOWN durably.
  auto final_store = OpenStore(database.Path());
  ASSERT_TRUE(final_store.has_value());

  risk::OrderRegistry final_registry;
  FixedAccountStateProvider final_account_state_provider(now);
  NoOpStartupReconciler final_reconciler;

  auto final_recovery = Recover(final_registry, *final_store, final_reconciler, final_account_state_provider, now);
  ASSERT_TRUE(final_recovery.has_value());
  EXPECT_EQ(final_registry.State(order.client_order_id), risk::OrderState::kRejected);
  EXPECT_EQ(final_registry.TotalReservedExposure(), risk::Money{});
}

TEST(SqliteSubmissionRecoveryTest, PreSubmitBusyDoesNotSubmitAndSameIntentCanRetrySafely) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();
  const auto order = ValidOrder(now);

  auto store = OpenStore(database.Path());
  ASSERT_TRUE(store.has_value());

  risk::RiskEngine engine(DefaultLimits());
  risk::OrderRegistry registry;
  FixedAccountStateProvider account_state_provider(now);
  NoOpStartupReconciler reconciler;
  FixedOutcomeSubmitter submitter(execution::SubmitOutcome::kOpen);

  auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());

  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  SqliteWriteLock lock(database.Path());
  ASSERT_TRUE(lock.Acquire());

  const auto blocked = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(blocked.status, execution::SubmissionStatus::kPreSubmitPersistenceFailed);
  EXPECT_EQ(submitter.SubmitCount(), 0U);
  EXPECT_FALSE(registry.Contains(order.client_order_id));
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});
  EXPECT_EQ(coordinator.GateState(), risk::GateState::kReady);

  lock.Release();

  const auto retry = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(retry.status, execution::SubmissionStatus::kSubmitted);
  EXPECT_EQ(retry.submit_outcome, execution::SubmitOutcome::kOpen);
  EXPECT_EQ(submitter.SubmitCount(), 1U);
  EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kOpen);
}

TEST(SqliteSubmissionRecoveryTest, PostSubmitBusyLeavesDurablePendingAndRestartFailsClosedUntilReconciled) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();
  const auto order = ValidOrder(now);

  {
    auto store = OpenStore(database.Path());
    ASSERT_TRUE(store.has_value());

    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;

    auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
    ASSERT_TRUE(recovery.has_value());

    SqliteWriteLock lock(database.Path());
    LockingSubmitter submitter(lock);
    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    const auto result = coordinator.ExecuteAtForTesting(order, now);

    ASSERT_TRUE(submitter.LockAcquiredDuringSubmit());
    EXPECT_EQ(result.status, execution::SubmissionStatus::kPostSubmitPersistenceFailed);
    EXPECT_EQ(result.submit_outcome, execution::SubmitOutcome::kOpen);
    EXPECT_EQ(submitter.SubmitCount(), 1U);
    EXPECT_EQ(registry.State(order.client_order_id), risk::OrderState::kOpen);
    EXPECT_EQ(coordinator.GateState(), risk::GateState::kReconciliationRequired);

    lock.Release();
  }

  // The in-memory OPEN transition was not persisted. The durable fallback
  // remains PENDING_SUBMIT and therefore cannot authorize a new runtime.
  {
    auto reopened_store = OpenStore(database.Path());
    ASSERT_TRUE(reopened_store.has_value());

    risk::OrderRegistry unresolved_registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;

    auto recovery = Recover(unresolved_registry, *reopened_store, reconciler, account_state_provider, now);

    ASSERT_FALSE(recovery.has_value());
    EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kUnresolvedOrdersRemain);
    EXPECT_EQ(unresolved_registry.State(order.client_order_id), risk::OrderState::kPendingSubmit);
    EXPECT_EQ(unresolved_registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
  }

  auto reopened_store = OpenStore(database.Path());
  ASSERT_TRUE(reopened_store.has_value());

  risk::RiskEngine restarted_engine(DefaultLimits());
  risk::OrderRegistry reconciled_registry;
  FixedAccountStateProvider account_state_provider(now);
  OpenUnresolvedStartupReconciler reconciler;
  FixedOutcomeSubmitter restarted_submitter(execution::SubmitOutcome::kOpen);

  auto recovery = Recover(reconciled_registry, *reopened_store, reconciler, account_state_provider, now);
  ASSERT_TRUE(recovery.has_value());
  ASSERT_EQ(reconciled_registry.State(order.client_order_id), risk::OrderState::kOpen);

  execution::OrderSubmissionCoordinator coordinator(restarted_engine, std::move(*recovery), restarted_submitter, MarketDataAt(now),
                                                    TradingDay(), TradingContext());
  const auto retry = coordinator.ExecuteAtForTesting(order, now);

  EXPECT_EQ(retry.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_TRUE(retry.risk_decision.HasReason(risk::RejectReason::kDuplicateClientOrderId));
  EXPECT_EQ(restarted_submitter.SubmitCount(), 0U);
}

TEST(SqliteSubmissionRecoveryTest, VenueQueryResolvesTimeoutBeforeAcceptAsDefinitivelyAbsent) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();
  const auto order = ValidOrder(now);
  StatefulFakeVenue venue(AmbiguousSubmitPoint::kBeforeAccept);

  {
    auto store = OpenStore(database.Path());
    ASSERT_TRUE(store.has_value());

    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;
    auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), venue, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);
    ASSERT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);
  }

  EXPECT_FALSE(venue.Contains(order.client_order_id));
  ASSERT_EQ(venue.SubmitCount(), 1U);

  auto reopened_store = OpenStore(database.Path());
  ASSERT_TRUE(reopened_store.has_value());

  risk::RiskEngine restarted_engine(DefaultLimits());
  risk::OrderRegistry restored_registry;
  FixedAccountStateProvider account_state_provider(now);
  execution::VenueStartupReconciler reconciler(venue);
  auto recovery = Recover(restored_registry, *reopened_store, reconciler, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());
  EXPECT_EQ(restored_registry.State(order.client_order_id), risk::OrderState::kRejected);
  EXPECT_EQ(restored_registry.TotalReservedExposure(), risk::Money{});
  EXPECT_EQ(venue.QueryCount(), 1U);

  execution::OrderSubmissionCoordinator coordinator(restarted_engine, std::move(*recovery), venue, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto retry = coordinator.ExecuteAtForTesting(order, now);
  EXPECT_EQ(retry.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_EQ(venue.SubmitCount(), 1U);
}

TEST(SqliteSubmissionRecoveryTest, VenueQueryFindsOrderAcceptedBeforeSubmitTimeout) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();
  const auto order = ValidOrder(now);
  StatefulFakeVenue venue(AmbiguousSubmitPoint::kAfterAccept);

  {
    auto store = OpenStore(database.Path());
    ASSERT_TRUE(store.has_value());

    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;
    auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), venue, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);
    ASSERT_EQ(registry.State(order.client_order_id), risk::OrderState::kUnknown);
  }

  ASSERT_TRUE(venue.Contains(order.client_order_id));
  ASSERT_EQ(venue.SubmitCount(), 1U);

  auto reopened_store = OpenStore(database.Path());
  ASSERT_TRUE(reopened_store.has_value());

  risk::RiskEngine restarted_engine(DefaultLimits());
  risk::OrderRegistry restored_registry;
  FixedAccountStateProvider account_state_provider(now);
  execution::VenueStartupReconciler reconciler(venue);
  auto recovery = Recover(restored_registry, *reopened_store, reconciler, account_state_provider, now);

  ASSERT_TRUE(recovery.has_value());
  EXPECT_EQ(restored_registry.State(order.client_order_id), risk::OrderState::kOpen);
  EXPECT_EQ(restored_registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));
  EXPECT_EQ(venue.QueryCount(), 1U);
  const auto restored_record = restored_registry.Record(order.client_order_id);
  ASSERT_TRUE(restored_record.has_value());
  EXPECT_EQ(restored_record->venue_order_id, "fake-venue-order-101");

  std::vector<execution::OrderAuditEvent> events;
  ASSERT_TRUE(reopened_store->LoadAuditEvents(events));
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].type, execution::OrderAuditEventType::kIntentPendingDurable);
  EXPECT_EQ(events[0].resulting_state, risk::OrderState::kPendingSubmit);
  EXPECT_EQ(events[1].type, execution::OrderAuditEventType::kVenueUnknown);
  EXPECT_EQ(events[1].resulting_state, risk::OrderState::kUnknown);
  EXPECT_EQ(events[2].type, execution::OrderAuditEventType::kReconciledOpen);
  EXPECT_EQ(events[2].resulting_state, risk::OrderState::kOpen);
  EXPECT_EQ(events[2].venue_order_id, "fake-venue-order-101");
  EXPECT_LT(events[0].sequence, events[1].sequence);
  EXPECT_LT(events[1].sequence, events[2].sequence);

  execution::OrderSubmissionCoordinator coordinator(restarted_engine, std::move(*recovery), venue, MarketDataAt(now), TradingDay(),
                                                    TradingContext());
  const auto retry = coordinator.ExecuteAtForTesting(order, now);
  EXPECT_EQ(retry.status, execution::SubmissionStatus::kRiskRejected);
  EXPECT_EQ(venue.SubmitCount(), 1U);
}

TEST(SqliteSubmissionRecoveryTest, UnavailableVenueQueryKeepsStartupFailClosed) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();
  const auto order = ValidOrder(now);
  StatefulFakeVenue venue(AmbiguousSubmitPoint::kAfterAccept);

  {
    auto store = OpenStore(database.Path());
    ASSERT_TRUE(store.has_value());

    risk::RiskEngine engine(DefaultLimits());
    risk::OrderRegistry registry;
    FixedAccountStateProvider account_state_provider(now);
    NoOpStartupReconciler reconciler;
    auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
    ASSERT_TRUE(recovery.has_value());

    execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), venue, MarketDataAt(now), TradingDay(),
                                                      TradingContext());
    ASSERT_EQ(coordinator.ExecuteAtForTesting(order, now).status, execution::SubmissionStatus::kSubmitted);
  }

  venue.SetQueryAvailable(false);
  auto reopened_store = OpenStore(database.Path());
  ASSERT_TRUE(reopened_store.has_value());

  risk::OrderRegistry restored_registry;
  FixedAccountStateProvider account_state_provider(now);
  execution::VenueStartupReconciler reconciler(venue);
  auto recovery = Recover(restored_registry, *reopened_store, reconciler, account_state_provider, now);

  ASSERT_FALSE(recovery.has_value());
  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kReconciliationFailed);
  EXPECT_EQ(restored_registry.State(order.client_order_id), risk::OrderState::kUnknown);
}

TEST(SqliteSubmissionRecoveryTest, UnderReservedDurableIdentityFailsStartupClosed) {
  TemporaryDatabase database;
  const auto now = risk::MonotonicClock::now();
  const auto order = ValidOrder(now);
  {
    auto store = OpenStore(database.Path());
    ASSERT_TRUE(store.has_value());
    ASSERT_TRUE(store->PersistTransition(
        risk::OrderRegistryEntry{
            .client_order_id = order.client_order_id,
            .state = risk::OrderState::kOpen,
            .market_id = order.market_id,
            .reserved_notional = risk::Money::FromWholeUsd(50),
            .reservation_active = true,
            .identity =
                risk::OrderIdentity{
                    .strategy_id = order.strategy_id,
                    .client_order_id = order.client_order_id,
                    .market_id = order.market_id,
                    .side = order.side,
                    .price = order.price,
                    .quantity = order.quantity,
                },
            .venue_order_id = "venue-order-101",
        },
        execution::OrderAuditEventType::kVenueOpen));
  }

  sqlite3* raw_database{};
  const auto path = database.Path().string();
  ASSERT_EQ(sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(raw_database, "UPDATE order_state SET reserved_notional = 10000 WHERE client_order_id = 101;", nullptr, nullptr,
                         nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(raw_database), SQLITE_OK);

  auto store = OpenStore(database.Path());
  ASSERT_TRUE(store.has_value());
  risk::OrderRegistry registry;
  FixedAccountStateProvider account_state_provider(now);
  NoOpStartupReconciler reconciler;
  const auto recovery = Recover(registry, *store, reconciler, account_state_provider, now);
  ASSERT_FALSE(recovery.has_value());
  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kRestoreFailed);
  EXPECT_TRUE(registry.Snapshot().empty());
}

}  // namespace
