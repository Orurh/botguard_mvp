#include "startup_recovery.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace botguard::execution {
namespace {

[[nodiscard]] bool HasUnresolvedStartupOrders(const risk::OrderRegistry& registry) {
  const auto entries = registry.Snapshot();

  return std::ranges::any_of(entries, [](const auto& entry) {
    return entry.state == risk::OrderState::kPendingSubmit || entry.state == risk::OrderState::kUnknown;
  });
}

[[nodiscard]] bool IsFresh(risk::MonotonicClock::time_point received_at, risk::MonotonicClock::time_point now,
                           std::chrono::milliseconds max_age) noexcept {
  if (max_age <= std::chrono::milliseconds::zero()) {
    return false;
  }

  const auto age = now - received_at;

  return age >= risk::MonotonicClock::duration::zero() && age <= max_age;
}

}  // namespace

std::expected<StartupRecoverySession, StartupRecoveryError> StartupRecoverySession::Recover(
    risk::OrderRegistry& registry, OrderStateStore& state_store, StartupReconciler& reconciler,
    AuthoritativeAccountStateProvider& account_state_provider, AccountStateSafetyPolicy account_state_policy,
    risk::MonotonicClock::time_point now) noexcept {
  return RecoverImpl(registry, state_store, reconciler, account_state_provider, account_state_policy, now);
}

std::expected<StartupRecoverySession, StartupRecoveryError> StartupRecoverySession::Recover(
    risk::OrderRegistry& registry, OrderStateStore& state_store, StartupReconciler& reconciler,
    AuthoritativeAccountStateProvider& account_state_provider, AccountStateSafetyPolicy account_state_policy) noexcept {
  return RecoverImpl(registry, state_store, reconciler, account_state_provider, account_state_policy, std::nullopt);
}

std::expected<StartupRecoverySession, StartupRecoveryError> StartupRecoverySession::RecoverImpl(
    risk::OrderRegistry& registry, OrderStateStore& state_store, StartupReconciler& reconciler,
    AuthoritativeAccountStateProvider& account_state_provider, AccountStateSafetyPolicy account_state_policy,
    std::optional<risk::MonotonicClock::time_point> validation_now) noexcept {
  std::vector<risk::OrderRegistryEntry> entries;

  if (!state_store.LoadAll(entries)) {
    return std::unexpected(StartupRecoveryError::kLoadFailed);
  }

  if (!registry.Restore(entries)) {
    return std::unexpected(StartupRecoveryError::kRestoreFailed);
  }

  ReconciliationCoordinator reconciliation(registry, state_store);

  if (!reconciler.Reconcile(reconciliation)) {
    return std::unexpected(StartupRecoveryError::kReconciliationFailed);
  }

  if (!reconciliation.Healthy()) {
    return std::unexpected(StartupRecoveryError::kReconciliationFailed);
  }

  if (HasUnresolvedStartupOrders(registry)) {
    return std::unexpected(StartupRecoveryError::kUnresolvedOrdersRemain);
  }

  // The reconciler's success is not sufficient by itself.
  // The authoritative account-state source must independently prove
  // that it has a usable account revision.
  const auto cursor = account_state_provider.CurrentCursor();

  if (!cursor) {
    return std::unexpected(StartupRecoveryError::kAccountStateUnavailable);
  }

  if (cursor->version.value == 0) {
    return std::unexpected(StartupRecoveryError::kInvalidAccountState);
  }

  const auto now = validation_now.value_or(risk::MonotonicClock::now());

  if (!IsFresh(cursor->received_at, now, account_state_policy.max_age)) {
    return std::unexpected(StartupRecoveryError::kStaleAccountState);
  }

  return StartupRecoverySession(registry, state_store, account_state_provider, account_state_policy, cursor->version);
}

}  // namespace botguard::execution
