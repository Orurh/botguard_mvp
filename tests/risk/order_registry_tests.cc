#include "risk/order_registry.h"

#include <gtest/gtest.h>

#include <array>

namespace {

namespace risk = botguard::risk;

bool BeginIntent(risk::OrderRegistry& registry, risk::ClientOrderId client_order_id = 101, risk::MarketId market_id = 10,
                 risk::Money reserved_notional = risk::Money::FromWholeUsd(25)) {
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

[[nodiscard]] risk::OrderRegistryEntry ValidEntry(risk::ClientOrderId client_order_id = 101, risk::MarketId market_id = 10) {
  return risk::OrderRegistryEntry{
      .client_order_id = client_order_id,
      .state = risk::OrderState::kUnknown,
      .market_id = market_id,
      .reserved_notional = risk::Money::FromWholeUsd(25),
      .reservation_active = true,
      .identity =
          risk::OrderIdentity{
              .strategy_id = 1,
              .client_order_id = client_order_id,
              .market_id = market_id,
              .side = risk::Side::kBuy,
              .price = risk::Price::FromCents(25),
              .quantity = risk::Quantity::FromWhole(100),
          },
      .venue_order_id = std::nullopt,
  };
}

TEST(OrderRegistryTest, BeginIntentCreatesPendingSubmitOrder) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  ASSERT_TRUE(registry.State(101).has_value());
  EXPECT_EQ(*registry.State(101), risk::OrderState::kPendingSubmit);
}

TEST(OrderRegistryTest, UnknownOrderDoesNotExist) {
  const risk::OrderRegistry registry;

  EXPECT_FALSE(registry.Contains(101));
  EXPECT_FALSE(registry.State(101).has_value());
}

TEST(OrderRegistryTest, CannotBeginSameIntentTwice) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));

  EXPECT_FALSE(BeginIntent(registry));
  EXPECT_EQ(registry.State(101), risk::OrderState::kPendingSubmit);
}

TEST(OrderRegistryTest, TimeoutMovesPendingOrderToUnknown) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkUnknown(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kUnknown);
}

TEST(OrderRegistryTest, UnknownIntentCannotBeStartedAgain) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkUnknown(101));

  EXPECT_FALSE(BeginIntent(registry));
  EXPECT_EQ(registry.State(101), risk::OrderState::kUnknown);
}

TEST(OrderRegistryTest, UnknownOrderCanReconcileToOpen) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkUnknown(101));

  ASSERT_TRUE(registry.MarkOpen(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kOpen);
}

TEST(OrderRegistryTest, UnknownOrderCanReconcileToFilled) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkUnknown(101));

  ASSERT_TRUE(registry.MarkFilled(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kFilled);
}

TEST(OrderRegistryTest, OpenOrderCanBecomeFilled) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkOpen(101));
  ASSERT_TRUE(registry.MarkFilled(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kFilled);
}

TEST(OrderRegistryTest, OpenOrderCanBecomeCancelled) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkOpen(101));
  ASSERT_TRUE(registry.MarkCancelled(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kCancelled);
}

TEST(OrderRegistryTest, RepeatedTransitionIsIdempotent) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkOpen(101));

  EXPECT_TRUE(registry.MarkOpen(101));
  EXPECT_EQ(registry.State(101), risk::OrderState::kOpen);
}

TEST(OrderRegistryTest, CannotTransitionUnknownClientOrderId) {
  risk::OrderRegistry registry;

  EXPECT_FALSE(registry.MarkOpen(101));
  EXPECT_FALSE(registry.MarkUnknown(101));
  EXPECT_FALSE(registry.MarkRejected(101));
}

TEST(OrderRegistryTest, FilledOrderIsTerminal) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkFilled(101));

  EXPECT_FALSE(registry.MarkOpen(101));
  EXPECT_FALSE(registry.MarkCancelled(101));
  EXPECT_FALSE(registry.MarkUnknown(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kFilled);
}

TEST(OrderRegistryTest, CancelledOrderIsTerminal) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkCancelled(101));

  EXPECT_FALSE(registry.MarkOpen(101));
  EXPECT_FALSE(registry.MarkFilled(101));
  EXPECT_FALSE(registry.MarkUnknown(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kCancelled);
}

TEST(OrderRegistryTest, RejectedOrderIsTerminal) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkRejected(101));

  EXPECT_FALSE(registry.MarkOpen(101));
  EXPECT_FALSE(registry.MarkFilled(101));
  EXPECT_FALSE(registry.MarkUnknown(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kRejected);
}

TEST(OrderRegistryTest, TerminalIntentCannotBeStartedAgain) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkRejected(101));

  EXPECT_FALSE(BeginIntent(registry));
  EXPECT_EQ(registry.State(101), risk::OrderState::kRejected);
}

TEST(OrderRegistryTest, BeginIntentReservesExposure) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry, 101, 10, risk::Money::FromWholeUsd(25)));

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(25));

  EXPECT_EQ(registry.MarketReservedExposure(10), risk::Money::FromWholeUsd(25));
}

TEST(OrderRegistryTest, UnknownKeepsReservation) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry, 101, 10, risk::Money::FromWholeUsd(25)));

  ASSERT_TRUE(registry.MarkUnknown(101));

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(25));

  EXPECT_EQ(registry.MarketReservedExposure(10), risk::Money::FromWholeUsd(25));
}

TEST(OrderRegistryTest, RejectedOrderReleasesReservation) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry, 101, 10, risk::Money::FromWholeUsd(25)));

  ASSERT_TRUE(registry.MarkRejected(101));

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  EXPECT_EQ(registry.MarketReservedExposure(10), risk::Money{});
}

TEST(OrderRegistryTest, CancelledOrderReleasesReservation) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry, 101, 10, risk::Money::FromWholeUsd(25)));

  ASSERT_TRUE(registry.MarkCancelled(101));

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  EXPECT_EQ(registry.MarketReservedExposure(10), risk::Money{});
}

TEST(OrderRegistryTest, FilledOrderKeepsReservationUntilExposureReconciled) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry, 101, 10, risk::Money::FromWholeUsd(25)));

  ASSERT_TRUE(registry.MarkFilled(101));

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(25));

  ASSERT_TRUE(registry.MarkExposureReconciled(101));

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});
}

TEST(OrderRegistryTest, CannotReleaseUnreconciledOpenReservation) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkOpen(101));

  EXPECT_FALSE(registry.MarkExposureReconciled(101));

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(25));
}

TEST(OrderRegistryTest, SnapshotRestorePreservesOrdersAndReservations) {
  risk::OrderRegistry source;

  ASSERT_TRUE(BeginIntent(source, 101, 10, risk::Money::FromWholeUsd(25)));

  ASSERT_TRUE(source.MarkUnknown(101));

  ASSERT_TRUE(BeginIntent(source, 102, 20, risk::Money::FromWholeUsd(15)));

  ASSERT_TRUE(source.MarkOpen(102));

  ASSERT_TRUE(BeginIntent(source, 103, 10, risk::Money::FromWholeUsd(30)));

  ASSERT_TRUE(source.MarkRejected(103));

  const auto snapshot = source.Snapshot();

  risk::OrderRegistry restored;

  ASSERT_TRUE(restored.Restore(snapshot));

  EXPECT_EQ(restored.State(101), risk::OrderState::kUnknown);

  EXPECT_EQ(restored.State(102), risk::OrderState::kOpen);

  EXPECT_EQ(restored.State(103), risk::OrderState::kRejected);

  EXPECT_EQ(restored.TotalReservedExposure(), risk::Money::FromWholeUsd(40));

  EXPECT_EQ(restored.MarketReservedExposure(10), risk::Money::FromWholeUsd(25));

  EXPECT_EQ(restored.MarketReservedExposure(20), risk::Money::FromWholeUsd(15));
}

TEST(OrderRegistryTest, SnapshotRestorePreservesReconciledFilledOrder) {
  risk::OrderRegistry source;

  ASSERT_TRUE(BeginIntent(source, 101, 10, risk::Money::FromWholeUsd(25)));

  ASSERT_TRUE(source.MarkFilled(101));

  ASSERT_TRUE(source.MarkExposureReconciled(101));

  ASSERT_EQ(source.TotalReservedExposure(), risk::Money{});

  const auto snapshot = source.Snapshot();

  risk::OrderRegistry restored;

  ASSERT_TRUE(restored.Restore(snapshot));

  EXPECT_EQ(restored.State(101), risk::OrderState::kFilled);

  EXPECT_EQ(restored.TotalReservedExposure(), risk::Money{});

  const auto record = restored.Record(101);

  ASSERT_TRUE(record.has_value());
  EXPECT_FALSE(record->reservation_active);
}

TEST(OrderRegistryTest, RestoreRejectsDuplicateIdsWithoutChangingCurrentState) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry, 999, 50, risk::Money::FromWholeUsd(10)));

  auto duplicate = ValidEntry();
  duplicate.state = risk::OrderState::kOpen;

  const std::array corrupted{ValidEntry(), duplicate};

  EXPECT_FALSE(registry.Restore(corrupted));

  // Existing state must survive a failed restore.
  EXPECT_TRUE(registry.Contains(999));

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(10));

  EXPECT_FALSE(registry.Contains(101));
}

TEST(OrderRegistryTest, RestoreRejectsUnknownOrderWithoutReservation) {
  risk::OrderRegistry registry;

  auto corrupted = ValidEntry();
  corrupted.reservation_active = false;

  EXPECT_FALSE(registry.Restore(std::array{corrupted}));
  EXPECT_FALSE(registry.Contains(101));
}

TEST(OrderRegistryTest, RestoreRejectsRejectedOrderWithActiveReservation) {
  risk::OrderRegistry registry;

  auto corrupted = ValidEntry();
  corrupted.state = risk::OrderState::kRejected;

  EXPECT_FALSE(registry.Restore(std::array{corrupted}));
}

TEST(OrderRegistryTest, RestoreRejectsZeroReservedNotional) {
  risk::OrderRegistry registry;

  auto corrupted = ValidEntry();
  corrupted.reserved_notional = risk::Money{};

  EXPECT_FALSE(registry.Restore(std::array{corrupted}));
}

TEST(OrderRegistryTest, RestoreRejectsUnderReservedDurableIdentity) {
  risk::OrderRegistry registry;

  auto corrupted = ValidEntry();
  corrupted.identity.price = risk::Price::FromCents(25);
  corrupted.identity.quantity = risk::Quantity::FromWhole(100);
  corrupted.reserved_notional = risk::Money::FromWholeUsd(24);

  EXPECT_FALSE(registry.Restore(std::array{corrupted}));
}

TEST(OrderRegistryTest, RestoreAllowsConservativeOverReservation) {
  risk::OrderRegistry registry;

  auto conservative = ValidEntry();
  conservative.identity.price = risk::Price::FromCents(25);
  conservative.identity.quantity = risk::Quantity::FromWhole(100);
  conservative.reserved_notional = risk::Money::FromWholeUsd(26);

  EXPECT_TRUE(registry.Restore(std::array{conservative}));
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(26));
}

TEST(OrderRegistryTest, RestoreRejectsOpenOrderWithoutReservation) {
  risk::OrderRegistry registry;

  auto corrupted = ValidEntry();
  corrupted.state = risk::OrderState::kOpen;
  corrupted.reservation_active = false;

  EXPECT_FALSE(registry.Restore(std::array{corrupted}));
}

TEST(OrderRegistryTest, RestoreRejectsInvalidSide) {
  auto corrupted = ValidEntry();
  corrupted.identity.side = static_cast<risk::Side>(255);

  risk::OrderRegistry registry;

  EXPECT_FALSE(registry.Restore(std::array{corrupted}));
}

TEST(OrderRegistryTest, AbortedBeforeSubmitReleasesReservationAndIsTerminal) {
  risk::OrderRegistry registry;

  ASSERT_TRUE(BeginIntent(registry, 101, 10, risk::Money::FromWholeUsd(25)));

  ASSERT_EQ(registry.TotalReservedExposure(), risk::Money::FromWholeUsd(25));

  ASSERT_TRUE(registry.MarkAbortedBeforeSubmit(101));

  EXPECT_EQ(registry.State(101), risk::OrderState::kAbortedBeforeSubmit);

  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});

  EXPECT_EQ(registry.MarketReservedExposure(10), risk::Money{});

  EXPECT_FALSE(registry.MarkOpen(101));
  EXPECT_FALSE(registry.MarkFilled(101));
  EXPECT_FALSE(registry.MarkUnknown(101));

  // Identity is still permanently claimed.
  EXPECT_FALSE(BeginIntent(registry, 101, 10, risk::Money::FromWholeUsd(25)));
}

TEST(OrderRegistryTest, SnapshotRestorePreservesAbortedBeforeSubmitOrder) {
  risk::OrderRegistry source;

  ASSERT_TRUE(BeginIntent(source, 101, 10, risk::Money::FromWholeUsd(25)));

  ASSERT_TRUE(source.MarkAbortedBeforeSubmit(101));

  const auto snapshot = source.Snapshot();

  risk::OrderRegistry restored;

  ASSERT_TRUE(restored.Restore(snapshot));

  EXPECT_EQ(restored.State(101), risk::OrderState::kAbortedBeforeSubmit);

  EXPECT_EQ(restored.TotalReservedExposure(), risk::Money{});

  const auto record = restored.Record(101);

  ASSERT_TRUE(record.has_value());
  EXPECT_FALSE(record->reservation_active);
}

TEST(OrderRegistryTest, RollbackUnpersistedIntentRejectsOpenUnknownAndTerminalOrders) {
  const auto expect_rejected = [](const auto& transition, risk::OrderState state) {
    risk::OrderRegistry registry;

    ASSERT_TRUE(BeginIntent(registry));

    ASSERT_TRUE(transition(registry));

    EXPECT_FALSE(registry.RollbackUnpersistedIntent(101));
    EXPECT_TRUE(registry.Contains(101));
    EXPECT_EQ(registry.State(101), state);
  };

  expect_rejected([](risk::OrderRegistry& registry) { return registry.MarkOpen(101); }, risk::OrderState::kOpen);
  expect_rejected([](risk::OrderRegistry& registry) { return registry.MarkUnknown(101); }, risk::OrderState::kUnknown);
  expect_rejected([](risk::OrderRegistry& registry) { return registry.MarkFilled(101); }, risk::OrderState::kFilled);
  expect_rejected([](risk::OrderRegistry& registry) { return registry.MarkCancelled(101); }, risk::OrderState::kCancelled);
  expect_rejected([](risk::OrderRegistry& registry) { return registry.MarkRejected(101); }, risk::OrderState::kRejected);
  expect_rejected([](risk::OrderRegistry& registry) { return registry.MarkAbortedBeforeSubmit(101); },
                  risk::OrderState::kAbortedBeforeSubmit);
}

TEST(OrderRegistryTest, VenueUpdateStoresAcknowledgementAndRejectsConflictingReplacement) {
  risk::OrderRegistry registry;
  ASSERT_TRUE(BeginIntent(registry));
  ASSERT_TRUE(registry.MarkUnknown(101));

  ASSERT_TRUE(registry.ApplyVenueUpdate(101, risk::OrderState::kOpen, std::string{"venue-order-1"}));

  const auto record = registry.Record(101);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, risk::OrderState::kOpen);
  EXPECT_EQ(record->venue_order_id, "venue-order-1");

  EXPECT_FALSE(registry.ApplyVenueUpdate(101, risk::OrderState::kFilled, std::string{"different-order"}));

  const auto unchanged = registry.Record(101);
  ASSERT_TRUE(unchanged.has_value());
  EXPECT_EQ(unchanged->state, risk::OrderState::kOpen);
  EXPECT_EQ(unchanged->venue_order_id, "venue-order-1");
}

}  // namespace
