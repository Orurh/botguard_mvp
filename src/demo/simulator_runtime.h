#pragma once

#include "agent/uds_agent_server.h"
#include "execution/trading_day_provider.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>

namespace botguard::demo {

class SimulatorRuntimeSafetyInputs : public execution::TradingDayProvider {
 public:
  virtual ~SimulatorRuntimeSafetyInputs() = default;
  [[nodiscard]] virtual std::optional<risk::Money> ObserveCurrentEquity() noexcept = 0;
};

struct SimulatorRuntimeConfig {
  std::filesystem::path order_database_path;
  std::filesystem::path venue_database_path;
  bool lose_submit_response{};
  std::shared_ptr<SimulatorRuntimeSafetyInputs> safety_inputs;
  bool crash_after_rollover_baseline_persisted{};
};

enum class SimulatorRuntimeError : std::uint8_t {
  kLeaseFailed,
  kStorageFailed,
  kVenueFailed,
  kControlStateFailed,
};

// One simulator runtime owns the same single execution thread and
// persist->submit->reconcile path used by a real venue composition root.
class SimulatorRuntime final : public agent::AgentRequestHandler {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<SimulatorRuntime>, SimulatorRuntimeError> Create(SimulatorRuntimeConfig config);

  ~SimulatorRuntime() override;
  SimulatorRuntime(const SimulatorRuntime&) = delete;
  SimulatorRuntime& operator=(const SimulatorRuntime&) = delete;
  SimulatorRuntime(SimulatorRuntime&&) = delete;
  SimulatorRuntime& operator=(SimulatorRuntime&&) = delete;

  [[nodiscard]] execution::SubmissionResult HandleSubmit(const risk::OrderIntent& intent) override;
  [[nodiscard]] agent::StatusSnapshot GetStatus() const override;
  [[nodiscard]] std::expected<agent::StatusSnapshot, agent::AgentControlError> Kill() override;
  [[nodiscard]] agent::ResumeResponse Resume() override;
  [[nodiscard]] std::optional<std::uint64_t> VenueSubmitCount() const noexcept;

 private:
  class Impl;
  explicit SimulatorRuntime(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace botguard::demo
