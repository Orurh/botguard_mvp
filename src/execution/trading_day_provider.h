#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace botguard::execution {

class TradingDayProvider {
 public:
  virtual ~TradingDayProvider() = default;

  [[nodiscard]] virtual std::optional<std::int64_t> CurrentUtcDay() noexcept = 0;
};

class SystemTradingDayProvider final : public TradingDayProvider {
 public:
  [[nodiscard]] std::optional<std::int64_t> CurrentUtcDay() noexcept override {
    return std::chrono::duration_cast<std::chrono::days>(std::chrono::system_clock::now().time_since_epoch()).count();
  }
};

}  // namespace botguard::execution
