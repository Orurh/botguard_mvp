#include "execution/reconciliation_coordinator.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

namespace execution = botguard::execution;
namespace risk = botguard::risk;

[[nodiscard]] bool BeginIntent(risk::OrderRegistry& registry, risk::ClientOrderId client_order_id = 101, risk::MarketId market_id = 10,
                               risk::Money reserved_notional = risk::Money::FromWholeUsd(50)) {
  const risk::OrderIntent intent{
      .strategy_id = 1,
      .client_order_id = client_order_id,
      .market_id = market_id,
      .side = risk::Side::kBuy,
      .price = risk::Price{.micros_per_unit = reserved_notional.micros},
      .quantity = risk::Quantity::FromWhole(1),
      .market_data_received_at = {},
  };

  return registry.BeginIntent(intent, reserved_notional);
}

class FakeOrderStateStore final : public execution::OrderStateStore {
 public:
  void FailOnUpsertCall(std::size_t call_number) noexcept { fail_on_upsert_call_ = call_number; }

  [[nodiscard]] bool PersistTransition(const risk::OrderRegistryEntry& entry, execution::OrderAuditEventType event_type) noexcept override {
    ++upsert_count_;

    if (fail_on_upsert_call_.has_value() && upsert_count_ == *fail_on_upsert_call_) {
      // Atomic failure contract:
      // previously committed state remains unchanged.
      return false;
    }

    committed_.insert_or_assign(entry.client_order_id, entry);
    events_.push_back(execution::OrderAuditEvent{
        .sequence = events_.size() + 1,
        .client_order_id = entry.client_order_id,
        .type = event_type,
        .resulting_state = entry.state,
        .venue_order_id = entry.venue_order_id,
    });

    return true;
  }

  [[nodiscard]] bool LoadAll(std::vector<risk::OrderRegistryEntry>& entries) noexcept override {
    std::vector<risk::OrderRegistryEntry> loaded;
    loaded.reserve(committed_.size());

    for (const auto& [client_order_id, entry] : committed_) {
      static_cast<void>(client_order_id);
      loaded.push_back(entry);
    }

    entries.swap(loaded);
    return true;
  }

  [[nodiscard]] bool LoadAuditEvents(std::vector<execution::OrderAuditEvent>& events) noexcept override {
    events = events_;
    return true;
  }

  [[nodiscard]] std::size_t UpsertCount() const noexcept { return upsert_count_; }

  [[nodiscard]] std::size_t CommittedCount() const noexcept { return committed_.size(); }

  [[nodiscard]]
  std::optional<risk::OrderRegistryEntry> CommittedEntry(risk::ClientOrderId client_order_id) const {
    const auto iterator = committed_.find(client_order_id);

    if (iterator == committed_.end()) {
      return std::nullopt;
    }

    return iterator->second;
  }

 private:
  std::optional<std::size_t> fail_on_upsert_call_;

  std::size_t upsert_count_{};

  std::unordered_map<risk::ClientOrderId, risk::OrderRegistryEntry> committed_;
  std::vector<execution::OrderAuditEvent> events_;
};

TEST(ReconciliationCoordinatorTest, RejectedTransitionIsPersistedBeforeSuccess) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkUnknown(101));

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkRejected(101), execution::ReconciliationUpdateStatus::kApplied);

  EXPECT_EQ(registry.State(101), risk::OrderState::kRejected);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  EXPECT_EQ(store.UpsertCount(), 1U);

  EXPECT_EQ(store.CommittedCount(), 1U);

  const auto committed = store.CommittedEntry(101);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->client_order_id, 101U);

  EXPECT_EQ(committed->state, risk::OrderState::kRejected);

  EXPECT_EQ(committed->market_id, 10U);

  EXPECT_EQ(committed->reserved_notional, risk::Money::FromWholeUsd(50));

  EXPECT_FALSE(committed->reservation_active);
}

TEST(ReconciliationCoordinatorTest, PersistenceFailureDoesNotReportApplied) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkUnknown(101));

  store.FailOnUpsertCall(1);

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkRejected(101), execution::ReconciliationUpdateStatus::kPersistenceFailed);

  // Runtime mutation already happened.
  EXPECT_EQ(registry.State(101), risk::OrderState::kRejected);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  // But the operation must not be reported as fully applied.
  EXPECT_EQ(store.UpsertCount(), 1U);

  EXPECT_EQ(store.CommittedCount(), 0U);
}

TEST(ReconciliationCoordinatorTest, FailedReplacementPreservesPreviousDurableState) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkUnknown(101));

  const auto initial_snapshot = registry.Snapshot();
  ASSERT_EQ(initial_snapshot.size(), 1U);
  ASSERT_TRUE(store.PersistTransition(initial_snapshot.front(), execution::OrderAuditEventType::kIntentPendingDurable));

  ASSERT_EQ(store.UpsertCount(), 1U);

  store.FailOnUpsertCall(2);

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkRejected(101), execution::ReconciliationUpdateStatus::kPersistenceFailed);

  // Runtime moved to the authoritative result.
  EXPECT_EQ(registry.State(101), risk::OrderState::kRejected);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  // Durable state remains the previous conservative UNKNOWN.
  const auto committed = store.CommittedEntry(101);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kUnknown);

  EXPECT_TRUE(committed->reservation_active);
}

TEST(ReconciliationCoordinatorTest, InvalidLifecycleTransitionDoesNotPersist) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkRejected(101));

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkOpen(101), execution::ReconciliationUpdateStatus::kRegistryMutationFailed);

  EXPECT_EQ(store.UpsertCount(), 0U);

  EXPECT_EQ(store.CommittedCount(), 0U);

  EXPECT_EQ(registry.State(101), risk::OrderState::kRejected);
}

TEST(ReconciliationCoordinatorTest, MissingOrderDoesNotPersist) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkRejected(999), execution::ReconciliationUpdateStatus::kRegistryMutationFailed);

  EXPECT_EQ(store.UpsertCount(), 0U);

  EXPECT_EQ(store.CommittedCount(), 0U);

  EXPECT_FALSE(registry.Contains(999));
}

TEST(ReconciliationCoordinatorTest, UnknownCanBeReconciledToOpenAndPersisted) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkUnknown(101));

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkOpen(101), execution::ReconciliationUpdateStatus::kApplied);

  EXPECT_EQ(registry.State(101), risk::OrderState::kOpen);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));

  const auto committed = store.CommittedEntry(101);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kOpen);

  EXPECT_TRUE(committed->reservation_active);
}

TEST(ReconciliationCoordinatorTest, UnknownCanBeReconciledToFilledAndPersisted) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkUnknown(101));

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkFilled(101), execution::ReconciliationUpdateStatus::kApplied);

  EXPECT_EQ(registry.State(101), risk::OrderState::kFilled);

  // FILLED remains reserved until authoritative account exposure
  // reconciliation explicitly releases it.
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));

  const auto committed = store.CommittedEntry(101);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kFilled);

  EXPECT_TRUE(committed->reservation_active);
}

TEST(ReconciliationCoordinatorTest, UnknownCanBeReconciledToCancelledAndPersisted) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkUnknown(101));

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkCancelled(101), execution::ReconciliationUpdateStatus::kApplied);

  EXPECT_EQ(registry.State(101), risk::OrderState::kCancelled);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  const auto committed = store.CommittedEntry(101);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kCancelled);

  EXPECT_FALSE(committed->reservation_active);
}

TEST(ReconciliationCoordinatorTest, ExposureReconciliationIsPersisted) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkFilled(101));

  ASSERT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkExposureReconciled(101), execution::ReconciliationUpdateStatus::kApplied);

  EXPECT_EQ(registry.State(101), risk::OrderState::kFilled);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  const auto committed = store.CommittedEntry(101);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kFilled);

  EXPECT_FALSE(committed->reservation_active);
}

TEST(ReconciliationCoordinatorTest, ExposurePersistenceFailureLeavesDurableReservationActive) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkFilled(101));

  // Establish the durable pre-reconciliation state.
  const auto initial_snapshot = registry.Snapshot();
  ASSERT_EQ(initial_snapshot.size(), 1U);
  ASSERT_TRUE(store.PersistTransition(initial_snapshot.front(), execution::OrderAuditEventType::kIntentPendingDurable));

  ASSERT_EQ(store.UpsertCount(), 1U);

  store.FailOnUpsertCall(2);

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkExposureReconciled(101), execution::ReconciliationUpdateStatus::kPersistenceFailed);

  // Runtime released capacity.
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  // Durable state remains conservative and still reserves exposure.
  const auto committed = store.CommittedEntry(101);

  ASSERT_TRUE(committed.has_value());

  EXPECT_EQ(committed->state, risk::OrderState::kFilled);

  EXPECT_TRUE(committed->reservation_active);
}

TEST(ReconciliationCoordinatorTest, ExposureReconciliationRejectsNonFilledOrder) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkOpen(101));

  execution::ReconciliationCoordinator reconciliation(registry, store);

  EXPECT_EQ(reconciliation.MarkExposureReconciled(101), execution::ReconciliationUpdateStatus::kRegistryMutationFailed);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(50));

  EXPECT_EQ(store.UpsertCount(), 0U);
}

TEST(ReconciliationCoordinatorTest, SnapshotExposesCurrentRegistryStateForReconciler) {
  FakeOrderStateStore store;

  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.MarkUnknown(101));

  ASSERT_TRUE(BeginIntent(registry, 102, 20, risk::Money::FromWholeUsd(25)));

  ASSERT_TRUE(registry.MarkOpen(102));

  execution::ReconciliationCoordinator reconciliation(registry, store);

  const auto snapshot = reconciliation.Snapshot();

  ASSERT_EQ(snapshot.size(), 2U);

  bool found_unknown = false;
  bool found_open = false;

  for (const auto& entry : snapshot) {
    if (entry.client_order_id == 101) {
      found_unknown = true;

      EXPECT_EQ(entry.state, risk::OrderState::kUnknown);

      EXPECT_TRUE(entry.reservation_active);
    }

    if (entry.client_order_id == 102) {
      found_open = true;

      EXPECT_EQ(entry.state, risk::OrderState::kOpen);

      EXPECT_TRUE(entry.reservation_active);
    }
  }

  EXPECT_TRUE(found_unknown);
  EXPECT_TRUE(found_open);
}

}  // namespace
