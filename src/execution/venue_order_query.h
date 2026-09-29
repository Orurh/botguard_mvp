#pragma once

#include "risk/order_registry.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

namespace botguard::execution {

enum class VenueOrderState : std::uint8_t {
  kOpen,
  kFilled,
  kCancelled,
  kRejected,
};

enum class VenueOrderQueryError : std::uint8_t {
  kUnavailable,
  kInvalidResponse,
};

// Successful nullopt means the venue authoritatively proved that no order
// exists for this durable identity/acknowledgement. A timeout, incomplete result, or an
// eventually-consistent lookup must be returned as an error instead.
struct VenueOrderLookupKey {
  risk::OrderIdentity identity{};
  std::optional<std::string> venue_order_id;
};

struct VenueOrderObservation {
  VenueOrderState state{VenueOrderState::kOpen};
  std::optional<std::string> venue_order_id;
};

using VenueOrderQueryResult = std::expected<std::optional<VenueOrderObservation>, VenueOrderQueryError>;

// Minimal V0 reconciliation query for one already-bound venue/account.
//
// An implementation should prefer venue_order_id when present. Without an
// acknowledgement it must use the complete original identity and may report
// definitive absence only when the venue/account API makes that conclusion
// authoritative.
class VenueOrderQuery {
 public:
  virtual ~VenueOrderQuery() = default;

  [[nodiscard]] virtual VenueOrderQueryResult Query(const VenueOrderLookupKey& key) noexcept = 0;
};

}  // namespace botguard::execution
