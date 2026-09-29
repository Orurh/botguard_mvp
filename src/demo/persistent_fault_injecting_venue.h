#pragma once

#include "execution/order_submitter.h"
#include "execution/venue_order_query.h"

#include <cstdint>
#include <filesystem>
#include <optional>

struct sqlite3;

namespace botguard::demo {

// Durable deterministic venue fixture for demos and process-level integration
// tests. It implements venue-neutral boundaries directly; it is not an HTTP
// server and makes no claim about a real venue's lookup consistency.
class PersistentFaultInjectingVenue final : public execution::OrderSubmitter, public execution::VenueOrderQuery {
 public:
  [[nodiscard]] static std::optional<PersistentFaultInjectingVenue> Open(const std::filesystem::path& path, bool lose_submit_response);

  ~PersistentFaultInjectingVenue() override;

  PersistentFaultInjectingVenue(const PersistentFaultInjectingVenue&) = delete;
  PersistentFaultInjectingVenue& operator=(const PersistentFaultInjectingVenue&) = delete;
  PersistentFaultInjectingVenue(PersistentFaultInjectingVenue&& other) noexcept;
  PersistentFaultInjectingVenue& operator=(PersistentFaultInjectingVenue&&) = delete;

  [[nodiscard]] execution::VenuePreflightResult Preflight(const risk::OrderIntent& order) const noexcept override;
  [[nodiscard]] execution::VenueSubmitResult Submit(const risk::OrderIntent& order) noexcept override;
  [[nodiscard]] execution::VenueOrderQueryResult Query(const execution::VenueOrderLookupKey& key) noexcept override;

  [[nodiscard]] std::optional<std::uint64_t> SubmitCount() const noexcept;

  [[nodiscard]] bool SetQueryAvailable(bool available) noexcept;
  void SetLoseSubmitResponse(bool lose) noexcept;

 private:
  PersistentFaultInjectingVenue(sqlite3* database, bool lose_submit_response) noexcept;

  sqlite3* database_{};
  bool lose_submit_response_{};
};

}  // namespace botguard::demo
