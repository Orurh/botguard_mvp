#include "demo/persistent_fault_injecting_venue.h"
#include "execution/market_data_freshness_provider.h"
#include "execution/order_submission_coordinator.h"
#include "execution/runtime_lease.h"
#include "execution/sqlite_storage_pair.h"
#include "execution/venue_startup_reconciler.h"
#include "risk/risk_engine.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

namespace execution = botguard::execution;
namespace risk = botguard::risk;
using botguard::demo::PersistentFaultInjectingVenue;

enum class CrashPoint : std::uint8_t {
  kNone,
  kAfterPendingDurable,
  kBeforeHttp,
  kAfterHttpReturn,
  kBeforeFinalPersist,
};

[[noreturn]] void InjectCrash() noexcept {
  static_cast<void>(::kill(::getpid(), SIGKILL));
  ::_exit(1);
}

class FixedAccountStateProvider final : public execution::AuthoritativeAccountStateProvider {
 public:
  explicit FixedAccountStateProvider(risk::MonotonicClock::time_point now) noexcept
      : cursor_{.version = {.value = 1}, .received_at = now}, snapshot_{.state = {}, .version = {.value = 1}, .received_at = now} {}

  [[nodiscard]] std::optional<execution::AccountStateCursor> CurrentCursor() const noexcept override { return cursor_; }
  [[nodiscard]] std::optional<execution::AccountStateSnapshot> Snapshot(risk::MarketId) const noexcept override { return snapshot_; }

 private:
  execution::AccountStateCursor cursor_;
  execution::AccountStateSnapshot snapshot_;
};

class FixedMarketDataProvider final : public execution::MarketDataFreshnessProvider {
 public:
  explicit FixedMarketDataProvider(risk::MonotonicClock::time_point now) noexcept
      : cursor_{.market_id = 10, .version = 1, .received_at = now} {}

  [[nodiscard]] std::optional<execution::MarketDataCursor> Current(risk::MarketId market_id) const noexcept override {
    return market_id == cursor_.market_id ? std::optional{cursor_} : std::nullopt;
  }

 private:
  execution::MarketDataCursor cursor_;
};

class CrashInjectingStore final : public execution::OrderStateStore {
 public:
  CrashInjectingStore(execution::OrderStateStore& delegate, CrashPoint point) noexcept : delegate_(delegate), point_(point) {}

  [[nodiscard]] bool PersistTransition(const risk::OrderRegistryEntry& entry, execution::OrderAuditEventType event_type) noexcept override {
    if (point_ == CrashPoint::kBeforeFinalPersist && entry.state != risk::OrderState::kPendingSubmit) {
      InjectCrash();
    }
    const auto persisted = delegate_.get().PersistTransition(entry, event_type);
    if (persisted && point_ == CrashPoint::kAfterPendingDurable && entry.state == risk::OrderState::kPendingSubmit) {
      InjectCrash();
    }
    return persisted;
  }

  [[nodiscard]] bool LoadAll(std::vector<risk::OrderRegistryEntry>& entries) noexcept override { return delegate_.get().LoadAll(entries); }

  [[nodiscard]] bool LoadAuditEvents(std::vector<execution::OrderAuditEvent>& events) noexcept override {
    return delegate_.get().LoadAuditEvents(events);
  }

 private:
  std::reference_wrapper<execution::OrderStateStore> delegate_;
  CrashPoint point_;
};

class CrashInjectingSubmitter final : public execution::OrderSubmitter {
 public:
  CrashInjectingSubmitter(PersistentFaultInjectingVenue& venue, CrashPoint point) noexcept : venue_(venue), point_(point) {}

  [[nodiscard]] execution::VenuePreflightResult Preflight(const risk::OrderIntent& order) const noexcept override {
    return venue_.get().Preflight(order);
  }

  [[nodiscard]] execution::VenueSubmitResult Submit(const risk::OrderIntent& order) noexcept override {
    if (point_ == CrashPoint::kBeforeHttp) {
      InjectCrash();
    }
    auto result = venue_.get().Submit(order);
    if (point_ == CrashPoint::kAfterHttpReturn) {
      InjectCrash();
    }
    return result;
  }

 private:
  std::reference_wrapper<PersistentFaultInjectingVenue> venue_;
  CrashPoint point_;
};

struct ScenarioPaths {
  std::filesystem::path order;
  std::filesystem::path runtime;
  std::filesystem::path venue;
};

[[nodiscard]] execution::RuntimeBinding Binding() {
  return execution::RuntimeBinding{.fingerprint = std::string(64, 'b')};
}

[[nodiscard]] execution::StorageGeneration Generation() {
  return execution::StorageGeneration{.value = std::string(64, 'c')};
}

[[nodiscard]] risk::RiskLimits Limits() noexcept {
  return risk::RiskLimits{
      .max_order_notional = risk::Money::FromWholeUsd(100),
      .max_market_gross_exposure = risk::Money::FromWholeUsd(500),
      .max_total_gross_exposure = risk::Money::FromWholeUsd(1'000),
      .max_loss_since_baseline = risk::Money::FromWholeUsd(100),
      .max_market_data_age = 5s,
  };
}

[[nodiscard]] risk::OrderIntent Intent() noexcept {
  return risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 1001,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(4),
      .market_data_received_at = {},
  };
}

[[nodiscard]] std::optional<execution::SqliteStoragePair> OpenStorage(const ScenarioPaths& paths) {
  execution::SystemTradingDayProvider day_provider;
  const auto current_day = day_provider.CurrentUtcDay();
  if (!current_day) {
    return std::nullopt;
  }
  const auto presence = execution::InspectStoragePair(paths.order, paths.runtime);
  if (!presence) {
    return std::nullopt;
  }
  if (*presence == execution::StoragePairPresence::kAbsent) {
    auto pair = execution::SqliteStoragePair::CreateNew(paths.order, paths.runtime, Binding(), Generation());
    if (!pair || !pair->RuntimeState().ResolveDailyBaselineEquity(*current_day, risk::Money::FromWholeUsd(1'000))) {
      return std::nullopt;
    }
    return std::optional<execution::SqliteStoragePair>{std::move(*pair)};
  }
  auto pair = execution::SqliteStoragePair::OpenExisting(paths.order, paths.runtime, Binding());
  if (!pair || !pair->RuntimeState().ResolveDailyBaselineEquity(*current_day, risk::Money::FromWholeUsd(1'000))) {
    return std::nullopt;
  }
  return std::optional<execution::SqliteStoragePair>{std::move(*pair)};
}

[[nodiscard]] bool RunInitialSubmission(const ScenarioPaths& paths, CrashPoint point, bool lose_response) {
  auto lease = execution::RuntimeLease::Acquire(paths.order);
  auto storage = OpenStorage(paths);
  auto venue = PersistentFaultInjectingVenue::Open(paths.venue, lose_response);
  if (!lease || !storage || !venue) {
    return false;
  }

  const auto now = risk::MonotonicClock::now();
  FixedAccountStateProvider account(now);
  FixedMarketDataProvider market_data(now);
  risk::OrderRegistry registry;
  risk::RiskEngine engine(Limits());
  execution::VenueStartupReconciler reconciler(*venue);
  CrashInjectingStore state_store(storage->OrderState(), point);
  auto recovery = execution::StartupRecoverySession::Recover(registry, state_store, reconciler, account,
                                                             execution::AccountStateSafetyPolicy{.max_age = 5s}, now);
  if (!recovery) {
    return false;
  }

  CrashInjectingSubmitter submitter(*venue, point);
  execution::SystemTradingDayProvider day_provider;
  const auto context = storage->RuntimeState().LoadTradingDayContext();
  if (!context || !*context) {
    return false;
  }
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), submitter, market_data, day_provider, **context);
  const auto result = coordinator.ExecuteAtForTesting(Intent(), now);
  return point == CrashPoint::kNone && result.status == execution::SubmissionStatus::kSubmitted &&
         result.submit_outcome == execution::SubmitOutcome::kUnknown &&
         registry.State(Intent().client_order_id) == risk::OrderState::kUnknown;
}

[[nodiscard]] bool RecoverAndProveNoDuplicate(const ScenarioPaths& paths, std::uint64_t expected_submit_count) {
  auto lease = execution::RuntimeLease::Acquire(paths.order);
  auto storage = OpenStorage(paths);
  auto venue = PersistentFaultInjectingVenue::Open(paths.venue, false);
  if (!lease || !storage || !venue) {
    return false;
  }

  const auto now = risk::MonotonicClock::now();
  FixedAccountStateProvider account(now);
  FixedMarketDataProvider market_data(now);
  risk::OrderRegistry registry;
  risk::RiskEngine engine(Limits());
  execution::VenueStartupReconciler reconciler(*venue);
  auto recovery = execution::StartupRecoverySession::Recover(registry, storage->OrderState(), reconciler, account,
                                                             execution::AccountStateSafetyPolicy{.max_age = 5s}, now);
  if (!recovery) {
    return false;
  }

  const auto expected_state = expected_submit_count == 0 ? risk::OrderState::kRejected : risk::OrderState::kOpen;
  if (registry.State(Intent().client_order_id) != expected_state) {
    return false;
  }

  execution::SystemTradingDayProvider day_provider;
  const auto context = storage->RuntimeState().LoadTradingDayContext();
  if (!context || !*context) {
    return false;
  }
  execution::OrderSubmissionCoordinator coordinator(engine, std::move(*recovery), *venue, market_data, day_provider, **context);
  const auto retry = coordinator.ExecuteAtForTesting(Intent(), now);
  const auto count = venue->SubmitCount();
  const auto passed = retry.status == execution::SubmissionStatus::kRiskRejected &&
                      retry.risk_decision.HasReason(risk::RejectReason::kDuplicateClientOrderId) && count &&
                      *count == expected_submit_count;
  if (passed) {
    std::cout << "  venue_submit_count=" << *count << '\n'
              << "  recovered_state=" << risk::ToString(expected_state) << '\n'
              << "  retry_result=DUPLICATE_CLIENT_ORDER_ID\n"
              << "  second_submit=false\n";
  }
  return passed;
}

[[nodiscard]] bool ProveUnavailableReconciliationFailsClosed(const ScenarioPaths& paths) {
  auto lease = execution::RuntimeLease::Acquire(paths.order);
  auto storage = OpenStorage(paths);
  auto venue = PersistentFaultInjectingVenue::Open(paths.venue, false);
  if (!lease || !storage || !venue) {
    return false;
  }
  if (!venue->SetQueryAvailable(false)) {
    return false;
  }

  const auto now = risk::MonotonicClock::now();
  FixedAccountStateProvider account(now);
  risk::OrderRegistry registry;
  execution::VenueStartupReconciler reconciler(*venue);
  const auto recovery = execution::StartupRecoverySession::Recover(registry, storage->OrderState(), reconciler, account,
                                                                   execution::AccountStateSafetyPolicy{.max_age = 5s}, now);
  const auto count = venue->SubmitCount();
  const auto passed = !recovery && recovery.error() == execution::StartupRecoveryError::kReconciliationFailed && count && *count == 1;
  if (passed) {
    std::cout << "  venue_submit_count=1\n"
              << "  recovery=FAIL_CLOSED\n"
              << "  trading_capability=ABSENT\n"
              << "  second_submit=false\n";
  }
  return passed;
}

[[nodiscard]] bool AuditContainsOnlyDurablePending(const ScenarioPaths& paths) {
  auto store = execution::SqliteOrderStateStore::OpenExisting(paths.order);
  if (!store) {
    return false;
  }
  std::vector<execution::OrderAuditEvent> events;
  return store->LoadAuditEvents(events) && events.size() == 1 && events.front().client_order_id == Intent().client_order_id &&
         events.front().type == execution::OrderAuditEventType::kIntentPendingDurable &&
         events.front().resulting_state == risk::OrderState::kPendingSubmit;
}

[[nodiscard]] bool RunAcceptedResponseLost(const ScenarioPaths& paths) {
  const auto child = ::fork();
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    ::_exit(RunInitialSubmission(paths, CrashPoint::kNone, true) ? 0 : 1);
  }

  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return false;
  }
  return RecoverAndProveNoDuplicate(paths, 1);
}

[[nodiscard]] bool RunReconciliationUnavailable(const ScenarioPaths& paths) {
  const auto child = ::fork();
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    ::_exit(RunInitialSubmission(paths, CrashPoint::kNone, true) ? 0 : 1);
  }

  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return false;
  }
  return ProveUnavailableReconciliationFailsClosed(paths);
}

[[nodiscard]] bool RunCrashCase(const ScenarioPaths& paths, CrashPoint point, std::uint64_t expected_submit_count) {
  const auto child = ::fork();
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    static_cast<void>(RunInitialSubmission(paths, point, false));
    ::_exit(1);
  }

  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    return false;
  }
  if (!AuditContainsOnlyDurablePending(paths)) {
    return false;
  }
  return RecoverAndProveNoDuplicate(paths, expected_submit_count);
}

[[nodiscard]] ScenarioPaths PathsFor(const std::filesystem::path& root, std::string_view name) {
  const auto base = root / (std::string(name) + ".db");
  return ScenarioPaths{.order = base, .runtime = base.string() + ".runtime", .venue = base.string() + ".venue"};
}

}  // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("botguard_failure_demo_" + std::to_string(::getpid()) + "_" +
                                                              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code error;
  if (!std::filesystem::create_directory(root, error) || error) {
    std::cerr << "cannot create demo workspace\n";
    return 1;
  }

  bool passed = true;
  const auto run = [&](std::string_view name, auto scenario) {
    std::cout << "scenario=" << name << '\n';
    const auto scenario_passed = scenario(PathsFor(root, name));
    std::cout << "  result=" << (scenario_passed ? "PASS" : "FAIL") << '\n';
    passed = passed && scenario_passed;
  };

  run("ACCEPTED_BUT_RESPONSE_LOST", RunAcceptedResponseLost);
  run("RECONCILIATION_UNAVAILABLE", RunReconciliationUnavailable);
  run("AFTER_PENDING_DURABLE", [](const ScenarioPaths& paths) { return RunCrashCase(paths, CrashPoint::kAfterPendingDurable, 0); });
  run("BEFORE_HTTP", [](const ScenarioPaths& paths) { return RunCrashCase(paths, CrashPoint::kBeforeHttp, 0); });
  run("AFTER_HTTP_RETURN", [](const ScenarioPaths& paths) { return RunCrashCase(paths, CrashPoint::kAfterHttpReturn, 1); });
  run("BEFORE_FINAL_PERSIST", [](const ScenarioPaths& paths) { return RunCrashCase(paths, CrashPoint::kBeforeFinalPersist, 1); });

  static_cast<void>(std::filesystem::remove_all(root, error));
  if (!passed) {
    std::cerr << "BotGuard failure-simulator demo failed\n";
    return 1;
  }
  std::cout << "submit_count_after_retry=1\n"
            << "duplicate_exchange_order=false\n"
            << "BotGuard accepted-response-loss and crash matrix passed.\n";
  return 0;
}
