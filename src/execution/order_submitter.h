#pragma once

#include "risk/types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace botguard::execution {

enum class VenuePreflightResult : std::uint8_t {
  kSupported,
  kUnsupported,
};

// Exchange-agnostic normalized result of one submit attempt.
enum class SubmitOutcome : std::uint8_t {
  kOpen,
  kFilled,
  kRejected,

  // The caller cannot prove whether the exchange accepted the order.
  // Includes timeout / connection loss / ambiguous transport failure.
  kUnknown,
};

struct VenueSubmitResult {
  SubmitOutcome outcome{SubmitOutcome::kUnknown};
  std::optional<std::string> venue_order_id;
};

// Exchange submission boundary.
//
// Exchange-specific adapters implement this interface and translate
// venue-specific responses into SubmitOutcome.
//
// Ambiguous transport failures must become kUnknown.
// Implementations must not throw across this interface.
class OrderSubmitter {
 public:
  virtual ~OrderSubmitter() = default;

  // Purely local venue-representability check. It must perform no I/O and no
  // external side effect. Unsupported intents are rejected before risk,
  // reservation, or durable PENDING_SUBMIT state is created.
  [[nodiscard]] virtual VenuePreflightResult Preflight(const risk::OrderIntent& order) const noexcept = 0;

  [[nodiscard]] virtual VenueSubmitResult Submit(const risk::OrderIntent& order) noexcept = 0;
};

}  // namespace botguard::execution
