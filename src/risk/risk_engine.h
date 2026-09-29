#pragma once

#include "types.h"

#include <atomic>

namespace botguard::risk {

class RiskEngine {
 public:
  explicit RiskEngine(RiskLimits limits);

  [[nodiscard]] RiskDecision Evaluate(const OrderIntent& order, const AccountState& state, MonotonicClock::time_point now) const noexcept;

  void SetKillSwitch(bool active) noexcept;

  [[nodiscard]] bool KillSwitchActive() const noexcept;
  [[nodiscard]] const RiskLimits& Limits() const noexcept;

 private:
  RiskLimits limits_;
  std::atomic_bool kill_switch_active_{false};
};

[[nodiscard]] const char* ToString(RejectReason reason) noexcept;

}  // namespace botguard::risk
