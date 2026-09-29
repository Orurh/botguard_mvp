#pragma once

#include "authoritative_account_state_provider.h"
#include "order_state_store.h"
#include "reconciliation_coordinator.h"

#include "risk/order_registry.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>

namespace botguard::execution {

// Startup reconciliation boundary.
//
// A successful implementation must:
// - query authoritative venue/account state;
// - resolve restored PENDING_SUBMIT / UNKNOWN orders through
//   ReconciliationCoordinator;
// - refresh/establish the authoritative account-state source before
//   returning success.
//
// Direct lifecycle mutation and direct persistence are intentionally
// not exposed through this interface.
//
// false means startup remains fail closed.
class StartupReconciler {
 public:
  virtual ~StartupReconciler() = default;

  [[nodiscard]] virtual bool Reconcile(ReconciliationCoordinator& reconciliation) noexcept = 0;
};

enum class StartupRecoveryError : std::uint8_t {
  kLoadFailed,
  kRestoreFailed,
  kReconciliationFailed,
  kUnresolvedOrdersRemain,

  kAccountStateUnavailable,
  kInvalidAccountState,
  kStaleAccountState,
};

// Move-only capability proving that startup recovery completed
// successfully for exactly this:
//
//   OrderRegistry
//   + OrderStateStore
//   + AuthoritativeAccountStateProvider
//
// There is intentionally no public constructor.
class StartupRecoverySession {
 public:
  StartupRecoverySession(const StartupRecoverySession&) = delete;

  StartupRecoverySession& operator=(const StartupRecoverySession&) = delete;

  StartupRecoverySession(StartupRecoverySession&&) noexcept = default;

  StartupRecoverySession& operator=(StartupRecoverySession&&) noexcept = default;

  [[nodiscard]] static std::expected<StartupRecoverySession, StartupRecoveryError> Recover(
      risk::OrderRegistry& registry, OrderStateStore& state_store, StartupReconciler& reconciler,
      AuthoritativeAccountStateProvider& account_state_provider, AccountStateSafetyPolicy account_state_policy,
      risk::MonotonicClock::time_point now) noexcept;

  // Production entry point. Freshness time is captured after Reconcile(), so
  // an account snapshot established by the reconciler cannot appear
  // future-dated merely because it was received during recovery.
  [[nodiscard]] static std::expected<StartupRecoverySession, StartupRecoveryError> Recover(
      risk::OrderRegistry& registry, OrderStateStore& state_store, StartupReconciler& reconciler,
      AuthoritativeAccountStateProvider& account_state_provider, AccountStateSafetyPolicy account_state_policy) noexcept;

  [[nodiscard]] risk::OrderRegistry& Registry() noexcept { return registry_.get(); }

  [[nodiscard]] const risk::OrderRegistry& Registry() const noexcept { return registry_.get(); }

  [[nodiscard]] OrderStateStore& StateStore() noexcept { return state_store_.get(); }

  [[nodiscard]]
  AuthoritativeAccountStateProvider& AccountStateProvider() noexcept {
    return account_state_provider_.get();
  }

  [[nodiscard]] AccountStateSafetyPolicy AccountStatePolicy() const noexcept { return account_state_policy_; }

  [[nodiscard]] AccountStateVersion RecoveredAccountStateVersion() const noexcept { return recovered_account_state_version_; }

 private:
  [[nodiscard]] static std::expected<StartupRecoverySession, StartupRecoveryError> RecoverImpl(
      risk::OrderRegistry& registry, OrderStateStore& state_store, StartupReconciler& reconciler,
      AuthoritativeAccountStateProvider& account_state_provider, AccountStateSafetyPolicy account_state_policy,
      std::optional<risk::MonotonicClock::time_point> validation_now) noexcept;

  StartupRecoverySession(risk::OrderRegistry& registry, OrderStateStore& state_store,
                         AuthoritativeAccountStateProvider& account_state_provider, AccountStateSafetyPolicy account_state_policy,
                         AccountStateVersion recovered_account_state_version) noexcept
      : registry_(registry),
        state_store_(state_store),
        account_state_provider_(account_state_provider),
        account_state_policy_(account_state_policy),
        recovered_account_state_version_(recovered_account_state_version) {}

  std::reference_wrapper<risk::OrderRegistry> registry_;

  std::reference_wrapper<OrderStateStore> state_store_;

  std::reference_wrapper<AuthoritativeAccountStateProvider> account_state_provider_;

  AccountStateSafetyPolicy account_state_policy_{};

  AccountStateVersion recovered_account_state_version_{};
};

}  // namespace botguard::execution
