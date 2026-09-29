#include "simulator_runtime.h"

#include "persistent_fault_injecting_venue.h"

#include "execution/market_data_freshness_provider.h"
#include "execution/runtime_lease.h"
#include "execution/sqlite_storage_pair.h"
#include "execution/trading_day_context.h"
#include "execution/venue_startup_reconciler.h"
#include "risk/risk_engine.h"

#include <signal.h>
#include <sys/random.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace botguard::demo {
namespace {

using namespace std::chrono_literals;

class SimulatorAccountState final : public execution::AuthoritativeAccountStateProvider {
 public:
  explicit SimulatorAccountState(SimulatorRuntimeSafetyInputs& inputs) noexcept : inputs_(inputs) {}

  void SetTradingDayContext(execution::TradingDayContext context) noexcept { context_ = context; }

  [[nodiscard]] bool Refresh() noexcept {
    if (!context_) {
      return false;
    }
    const auto equity = inputs_.ObserveCurrentEquity();
    if (!equity || equity->micros < 0) {
      cursor_.reset();
      snapshot_.reset();
      return false;
    }
    const auto now = risk::MonotonicClock::now();
    const execution::AccountStateVersion version{.value = ++version_};
    cursor_ = execution::AccountStateCursor{.version = version, .received_at = now};
    snapshot_ = execution::AccountStateSnapshot{
        .state = risk::AccountState{.pnl_since_baseline = risk::Money{.micros = equity->micros - context_->baseline_equity.micros}},
        .version = version,
        .received_at = now,
    };
    return true;
  }

  [[nodiscard]] std::optional<execution::AccountStateCursor> CurrentCursor() const noexcept override { return cursor_; }
  [[nodiscard]] std::optional<execution::AccountStateSnapshot> Snapshot(risk::MarketId) const noexcept override { return snapshot_; }

 private:
  SimulatorRuntimeSafetyInputs& inputs_;
  std::optional<execution::TradingDayContext> context_;
  std::uint64_t version_{};
  std::optional<execution::AccountStateCursor> cursor_;
  std::optional<execution::AccountStateSnapshot> snapshot_;
};

class SystemSimulatorRuntimeSafetyInputs final : public SimulatorRuntimeSafetyInputs {
 public:
  [[nodiscard]] std::optional<std::int64_t> CurrentUtcDay() noexcept override {
    return std::chrono::duration_cast<std::chrono::days>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  [[nodiscard]] std::optional<risk::Money> ObserveCurrentEquity() noexcept override { return risk::Money::FromWholeUsd(1'000); }
};

class SimulatorMarketData final : public execution::MarketDataFreshnessProvider {
 public:
  void Refresh(risk::MarketId market_id) noexcept {
    cursor_ = execution::MarketDataCursor{.market_id = market_id, .version = ++version_, .received_at = risk::MonotonicClock::now()};
  }

  [[nodiscard]] std::optional<execution::MarketDataCursor> Current(risk::MarketId market_id) const noexcept override {
    return cursor_.version != 0 && cursor_.market_id == market_id ? std::optional{cursor_} : std::nullopt;
  }

 private:
  std::uint64_t version_{};
  execution::MarketDataCursor cursor_{};
};

[[nodiscard]] risk::RiskLimits Limits() noexcept {
  return risk::RiskLimits{
      .max_order_notional = risk::Money::FromWholeUsd(100),
      .max_market_gross_exposure = risk::Money::FromWholeUsd(500),
      .max_total_gross_exposure = risk::Money::FromWholeUsd(1'000),
      .max_loss_since_baseline = risk::Money::FromWholeUsd(100),
      .max_market_data_age = 5s,
  };
}

[[nodiscard]] execution::RuntimeBinding Binding() {
  return execution::RuntimeBinding{.fingerprint = std::string(64, 'd')};
}

[[nodiscard]] std::optional<execution::StorageGeneration> NewGeneration() noexcept {
  std::array<unsigned char, 32> random{};
  if (::getrandom(random.data(), random.size(), 0) != static_cast<ssize_t>(random.size())) {
    return std::nullopt;
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string value(random.size() * 2, '0');
  for (std::size_t index = 0; index < random.size(); ++index) {
    value[index * 2] = hex[random[index] >> 4U];
    value[index * 2 + 1] = hex[random[index] & 0x0FU];
  }
  return execution::StorageGeneration{.value = std::move(value)};
}

[[nodiscard]] std::optional<execution::SqliteStoragePair> OpenStorage(const std::filesystem::path& order_path) {
  auto runtime_path = order_path;
  runtime_path += ".runtime";
  const auto presence = execution::InspectStoragePair(order_path, runtime_path);
  if (!presence) {
    return std::nullopt;
  }

  std::expected<execution::SqliteStoragePair, execution::SqliteStoragePairError> pair =
      std::unexpected(execution::SqliteStoragePairError::kOrderStoreFailed);
  if (*presence == execution::StoragePairPresence::kAbsent) {
    const auto generation = NewGeneration();
    if (!generation) {
      return std::nullopt;
    }
    pair = execution::SqliteStoragePair::CreateNew(order_path, runtime_path, Binding(), *generation);
  } else {
    pair = execution::SqliteStoragePair::OpenExisting(order_path, runtime_path, Binding());
  }
  if (!pair) {
    return std::nullopt;
  }
  return std::move(*pair);
}

}  // namespace

class SimulatorRuntime::Impl final {
 public:
  Impl(execution::RuntimeLease lease, execution::SqliteStoragePair storage, PersistentFaultInjectingVenue venue,
       std::shared_ptr<SimulatorRuntimeSafetyInputs> safety_inputs, bool crash_after_rollover_baseline_persisted)
      : lease_(std::move(lease)),
        storage_(std::move(storage)),
        venue_(std::move(venue)),
        safety_inputs_(std::move(safety_inputs)),
        account_(*safety_inputs_),
        engine_(Limits()),
        crash_after_rollover_baseline_persisted_(crash_after_rollover_baseline_persisted) {
    engine_.SetKillSwitch(true);
  }

  struct CandidateContext {
    std::unique_ptr<risk::OrderRegistry> registry;
    std::unique_ptr<execution::OrderSubmissionCoordinator> coordinator;
  };

  [[nodiscard]] std::optional<CandidateContext> RecoverCandidate() {
    if (!active_trading_day_context_) {
      return std::nullopt;
    }
    auto candidate_registry = std::make_unique<risk::OrderRegistry>();
    execution::VenueStartupReconciler reconciler(venue_);
    auto recovery = execution::StartupRecoverySession::Recover(*candidate_registry, storage_.OrderState(), reconciler, account_,
                                                               execution::AccountStateSafetyPolicy{.max_age = 5s});
    if (!recovery) {
      UpdateDurableMetrics();
      return std::nullopt;
    }
    auto candidate_coordinator = std::make_unique<execution::OrderSubmissionCoordinator>(
        engine_, std::move(*recovery), venue_, market_data_, *safety_inputs_, *active_trading_day_context_);
    return CandidateContext{.registry = std::move(candidate_registry), .coordinator = std::move(candidate_coordinator)};
  }

  void CloseAdmissionForRecovery() {
    engine_.SetKillSwitch(true);
    UpdateMetricsFromRegistry();
    coordinator_.reset();
    registry_.reset();
    active_trading_day_context_.reset();
  }

  [[nodiscard]] bool PrepareTradingDay() {
    const auto current_day = safety_inputs_->CurrentUtcDay();
    if (!current_day || *current_day < 0) {
      CloseAdmissionForRecovery();
      return false;
    }
    const auto stored = storage_.RuntimeState().LoadTradingDayContext();
    if (!stored || (*stored && (*stored)->utc_day > *current_day)) {
      CloseAdmissionForRecovery();
      return false;
    }

    auto context = *stored;
    const auto rollover = !context || context->utc_day < *current_day;
    if (rollover) {
      CloseAdmissionForRecovery();
      const auto authoritative_equity = safety_inputs_->ObserveCurrentEquity();
      if (!authoritative_equity || authoritative_equity->micros < 0) {
        return false;
      }
      const auto baseline = storage_.RuntimeState().ResolveDailyBaselineEquity(*current_day, *authoritative_equity);
      if (!baseline) {
        return false;
      }
      context = execution::TradingDayContext{.utc_day = *current_day, .baseline_equity = *baseline};
      if (*stored && crash_after_rollover_baseline_persisted_) {
        static_cast<void>(::kill(::getpid(), SIGKILL));
        ::_exit(86);
      }
    }

    account_.SetTradingDayContext(*context);
    if (!account_.Refresh()) {
      CloseAdmissionForRecovery();
      return false;
    }
    active_trading_day_context_ = *context;
    return true;
  }

  [[nodiscard]] bool Initialize() {
    const auto killed = storage_.RuntimeState().OperatorKilled();
    if (!killed) {
      return false;
    }
    operator_killed_ = *killed;
    UpdateDurableMetrics();
    if (PrepareTradingDay()) {
      auto candidate = RecoverCandidate();
      if (!candidate) {
        return true;
      }
      Publish(std::move(*candidate));
    }
    if (!operator_killed_ && coordinator_) {
      engine_.SetKillSwitch(false);
    }
    return true;
  }

  [[nodiscard]] execution::SubmissionResult Handle(const risk::OrderIntent& intent) {
    if (State() != agent::AgentState::kReady) {
      return BlockedSubmission();
    }
    if (!PrepareTradingDay()) {
      return BlockedSubmission();
    }
    if (!coordinator_) {
      auto candidate = RecoverCandidate();
      if (!candidate) {
        return BlockedSubmission();
      }
      Publish(std::move(*candidate));
      engine_.SetKillSwitch(false);
    }
    market_data_.Refresh(intent.market_id);
    const auto result = coordinator_->Execute(intent);
    UpdateMetricsFromRegistry();
    return result;
  }

  [[nodiscard]] agent::StatusSnapshot Status() const {
    return agent::StatusSnapshot{
        .agent_state = State(),
        .kill_switch_active = engine_.KillSwitchActive(),
        .gate_state = coordinator_ && coordinator_->GateState() == risk::GateState::kReady ? agent::WireGateState::kReady
                                                                                           : agent::WireGateState::kReconciliationRequired,
        .durable_order_count = durable_order_count_,
        .active_reservation_count = active_reservation_count_,
        .total_reserved_exposure = total_reserved_exposure_,
    };
  }

  [[nodiscard]] std::expected<agent::StatusSnapshot, agent::AgentControlError> Kill() {
    engine_.SetKillSwitch(true);
    if (!storage_.RuntimeState().SetOperatorKilled(true)) {
      return std::unexpected(agent::AgentControlError::kPersistenceFailed);
    }
    operator_killed_ = true;
    return Status();
  }

  [[nodiscard]] agent::ResumeResponse Resume() {
    if (State() == agent::AgentState::kReady) {
      return agent::ResumeResponse{.result = agent::WireResumeResult::kAlreadyReady, .status = Status()};
    }
    CloseAdmissionForRecovery();
    if (!PrepareTradingDay()) {
      return agent::ResumeResponse{.result = agent::WireResumeResult::kRecoveryFailed, .status = Status()};
    }
    auto candidate = RecoverCandidate();
    if (!candidate) {
      return agent::ResumeResponse{.result = agent::WireResumeResult::kRecoveryFailed, .status = Status()};
    }
    if (!storage_.RuntimeState().SetOperatorKilled(false)) {
      return agent::ResumeResponse{.result = agent::WireResumeResult::kPersistenceFailed, .status = Status()};
    }
    Publish(std::move(*candidate));
    operator_killed_ = false;
    engine_.SetKillSwitch(false);
    return agent::ResumeResponse{.result = agent::WireResumeResult::kReady, .status = Status()};
  }

  [[nodiscard]] agent::AgentState State() const noexcept {
    if (operator_killed_) {
      return agent::AgentState::kKilled;
    }
    if (!coordinator_ || coordinator_->GateState() != risk::GateState::kReady) {
      return agent::AgentState::kRecoveryRequired;
    }
    return engine_.KillSwitchActive() ? agent::AgentState::kKilled : agent::AgentState::kReady;
  }

  [[nodiscard]] execution::SubmissionResult BlockedSubmission() const {
    risk::RiskDecision decision;
    decision.AddReason(State() == agent::AgentState::kKilled ? risk::RejectReason::kKillSwitchActive
                                                             : risk::RejectReason::kReconciliationRequired);
    return execution::SubmissionResult{
        .status = execution::SubmissionStatus::kRiskRejected,
        .risk_decision = decision,
        .submit_outcome = std::nullopt,
    };
  }

  void Publish(CandidateContext candidate) {
    registry_ = std::move(candidate.registry);
    coordinator_ = std::move(candidate.coordinator);
    UpdateMetricsFromRegistry();
  }

  void UpdateMetricsFromRegistry() {
    if (!registry_) {
      return;
    }
    const auto entries = registry_->Snapshot();
    durable_order_count_ = entries.size();
    active_reservation_count_ = 0;
    for (const auto& entry : entries) {
      if (entry.reservation_active) {
        ++active_reservation_count_;
      }
    }
    total_reserved_exposure_ = registry_->TotalReservedExposure();
  }

  void UpdateDurableMetrics() {
    std::vector<risk::OrderRegistryEntry> entries;
    if (!storage_.OrderState().LoadAll(entries)) {
      return;
    }
    durable_order_count_ = entries.size();
    active_reservation_count_ = 0;
    std::int64_t total{};
    for (const auto& entry : entries) {
      if (entry.reservation_active) {
        ++active_reservation_count_;
        if (entry.reserved_notional.micros < 0 || total > std::numeric_limits<std::int64_t>::max() - entry.reserved_notional.micros) {
          total = std::numeric_limits<std::int64_t>::max();
        } else {
          total += entry.reserved_notional.micros;
        }
      }
    }
    total_reserved_exposure_.micros = total;
  }

  execution::RuntimeLease lease_;
  execution::SqliteStoragePair storage_;
  PersistentFaultInjectingVenue venue_;
  std::shared_ptr<SimulatorRuntimeSafetyInputs> safety_inputs_;
  SimulatorAccountState account_;
  SimulatorMarketData market_data_;
  risk::RiskEngine engine_;
  std::optional<execution::TradingDayContext> active_trading_day_context_;
  std::unique_ptr<risk::OrderRegistry> registry_;
  std::unique_ptr<execution::OrderSubmissionCoordinator> coordinator_;
  bool operator_killed_{};
  std::uint64_t durable_order_count_{};
  std::uint64_t active_reservation_count_{};
  risk::Money total_reserved_exposure_{};
  bool crash_after_rollover_baseline_persisted_{};
};

std::expected<std::unique_ptr<SimulatorRuntime>, SimulatorRuntimeError> SimulatorRuntime::Create(SimulatorRuntimeConfig config) {
  auto lease = execution::RuntimeLease::Acquire(config.order_database_path);
  if (!lease) {
    return std::unexpected(SimulatorRuntimeError::kLeaseFailed);
  }
  auto storage = OpenStorage(config.order_database_path);
  if (!storage) {
    return std::unexpected(SimulatorRuntimeError::kStorageFailed);
  }
  auto venue = PersistentFaultInjectingVenue::Open(config.venue_database_path, config.lose_submit_response);
  if (!venue) {
    return std::unexpected(SimulatorRuntimeError::kVenueFailed);
  }
  if (!config.safety_inputs) {
    config.safety_inputs = std::make_shared<SystemSimulatorRuntimeSafetyInputs>();
  }
  auto impl = std::make_unique<Impl>(std::move(*lease), std::move(*storage), std::move(*venue), std::move(config.safety_inputs),
                                     config.crash_after_rollover_baseline_persisted);
  if (!impl->Initialize()) {
    return std::unexpected(SimulatorRuntimeError::kControlStateFailed);
  }
  return std::unique_ptr<SimulatorRuntime>(new SimulatorRuntime(std::move(impl)));
}

SimulatorRuntime::~SimulatorRuntime() = default;

SimulatorRuntime::SimulatorRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

execution::SubmissionResult SimulatorRuntime::HandleSubmit(const risk::OrderIntent& intent) {
  return impl_->Handle(intent);
}

agent::StatusSnapshot SimulatorRuntime::GetStatus() const {
  return impl_->Status();
}

std::expected<agent::StatusSnapshot, agent::AgentControlError> SimulatorRuntime::Kill() {
  return impl_->Kill();
}

agent::ResumeResponse SimulatorRuntime::Resume() {
  return impl_->Resume();
}

std::optional<std::uint64_t> SimulatorRuntime::VenueSubmitCount() const noexcept {
  return impl_->venue_.SubmitCount();
}

}  // namespace botguard::demo
