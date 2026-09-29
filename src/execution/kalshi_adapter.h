#pragma once

#include "authoritative_account_state_provider.h"
#include "market_data_freshness_provider.h"
#include "order_submitter.h"
#include "trading_day_context.h"
#include "venue_order_query.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace botguard::execution {

struct KalshiMarketBinding {
  risk::MarketId market_id{};
  std::string ticker;
};

struct KalshiAdapterConfig {
  // Demo: https://external-api.demo.kalshi.co/trade-api/v2
  // Production: https://external-api.kalshi.com/trade-api/v2
  std::string base_url;
  std::string api_key_id;
  std::string private_key_pem;
  std::vector<KalshiMarketBinding> markets;
  std::uint32_t subaccount{};
  std::chrono::milliseconds request_timeout{5'000};
};

enum class KalshiAdapterCreateError : std::uint8_t {
  kInvalidConfig,
  kInvalidPrivateKey,
  kTransportInitializationFailed,
};

// One adapter instance is permanently bound to one Kalshi account/subaccount.
// V0 deployment invariant: the subaccount is controlled exclusively by this
// BotGuard process. Manual or external order submission is forbidden because
// resting external orders are not represented by the local reservation ledger.
// It is synchronous and not thread-safe; the owning execution thread calls it.
class KalshiAdapter final : public OrderSubmitter,
                            public VenueOrderQuery,
                            public AuthoritativeAccountStateProvider,
                            public MarketDataFreshnessProvider {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<KalshiAdapter>, KalshiAdapterCreateError> Create(KalshiAdapterConfig config) noexcept;

  ~KalshiAdapter() override;

  KalshiAdapter(const KalshiAdapter&) = delete;
  KalshiAdapter& operator=(const KalshiAdapter&) = delete;
  KalshiAdapter(KalshiAdapter&&) = delete;
  KalshiAdapter& operator=(KalshiAdapter&&) = delete;

  [[nodiscard]] VenuePreflightResult Preflight(const risk::OrderIntent& order) const noexcept override;
  [[nodiscard]] VenueSubmitResult Submit(const risk::OrderIntent& order) noexcept override;
  [[nodiscard]] VenueOrderQueryResult Query(const VenueOrderLookupKey& key) noexcept override;

  // Fetches authoritative total equity without publishing account state.
  [[nodiscard]] std::optional<risk::Money> ObserveCurrentEquity() noexcept;

  // Refreshes the complete authoritative account snapshot used by risk.
  // pnl_since_baseline is derived from the explicitly supplied durable
  // per-day baseline context; it is not calendar-day PnL.
  // Failure invalidates the cache so callers fail closed.
  [[nodiscard]] bool RefreshAccountState(const TradingDayContext& context) noexcept;

  // Fetches and validates a venue orderbook snapshot, then records its local
  // receive time as trusted freshness evidence for this market.
  [[nodiscard]] bool RefreshMarketData(risk::MarketId market_id) noexcept;

  [[nodiscard]] std::optional<AccountStateCursor> CurrentCursor() const noexcept override;
  [[nodiscard]] std::optional<AccountStateSnapshot> Snapshot(risk::MarketId market_id) const noexcept override;
  [[nodiscard]] std::optional<MarketDataCursor> Current(risk::MarketId market_id) const noexcept override;

 private:
  class Impl;

  explicit KalshiAdapter(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace botguard::execution
