#include "kalshi_adapter.h"
#include "kalshi/codec.h"
#include "kalshi/signed_http_client.h"

#include <chrono>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace botguard::execution {
namespace {

using kalshi_internal::CheckedAdd;
using kalshi_internal::ClientOrderUuid;
using kalshi_internal::DecodeOrderState;
using kalshi_internal::FormatFixed;
using kalshi_internal::IsSafeTicker;
using kalshi_internal::JsonText;
using kalshi_internal::ParseFixedMicros;
using kalshi_internal::ParseJson;
using kalshi_internal::ValidateOrderbookSide;

}  // namespace

class KalshiAdapter::Impl {
 public:
  Impl(KalshiAdapterConfig config, std::unique_ptr<kalshi_internal::SignedHttpClient> http)
      : http_(std::move(http)), subaccount_(config.subaccount) {
    for (auto& binding : config.markets) {
      tickers_.emplace(binding.market_id, binding.ticker);
      market_ids_.emplace(std::move(binding.ticker), binding.market_id);
    }
  }

  [[nodiscard]] const std::string* Ticker(risk::MarketId market_id) const noexcept {
    const auto iterator = tickers_.find(market_id);
    return iterator == tickers_.end() ? nullptr : &iterator->second;
  }

  void InvalidateAccountState() noexcept {
    cursor_.reset();
    snapshots_.clear();
  }

  std::unique_ptr<kalshi_internal::SignedHttpClient> http_;
  std::uint32_t subaccount_{};
  std::unordered_map<risk::MarketId, std::string> tickers_;
  std::unordered_map<std::string, risk::MarketId> market_ids_;
  std::optional<AccountStateCursor> cursor_;
  std::unordered_map<risk::MarketId, AccountStateSnapshot> snapshots_;
  std::unordered_map<risk::MarketId, MarketDataCursor> market_data_cursors_;
  std::uint64_t next_version_{1};
  std::uint64_t next_market_data_version_{1};
  std::optional<TradingDayContext> last_published_context_;
};

KalshiAdapter::KalshiAdapter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
KalshiAdapter::~KalshiAdapter() = default;

VenuePreflightResult KalshiAdapter::Preflight(const risk::OrderIntent& order) const noexcept {
  const auto* ticker = impl_->Ticker(order.market_id);
  const auto representable = ticker != nullptr && order.side == risk::Side::kBuy && order.price.micros_per_unit > 0 &&
                             order.price.micros_per_unit < risk::kMicrosPerUsd && order.price.micros_per_unit % 100 == 0 &&
                             order.quantity.microunits > 0 && order.quantity.microunits % 10'000 == 0;
  return representable ? VenuePreflightResult::kSupported : VenuePreflightResult::kUnsupported;
}

std::expected<std::unique_ptr<KalshiAdapter>, KalshiAdapterCreateError> KalshiAdapter::Create(KalshiAdapterConfig config) noexcept {
  try {
    if (config.base_url.empty() || config.base_url.back() == '/' || config.api_key_id.empty() || config.private_key_pem.empty() ||
        config.markets.empty() || config.subaccount > 32 || config.request_timeout <= std::chrono::milliseconds::zero() ||
        config.request_timeout > std::chrono::minutes(5)) {
      return std::unexpected(KalshiAdapterCreateError::kInvalidConfig);
    }
    std::unordered_map<risk::MarketId, bool> ids;
    std::unordered_map<std::string, bool> tickers;
    for (const auto& binding : config.markets) {
      if (!IsSafeTicker(binding.ticker) || !ids.emplace(binding.market_id, true).second || !tickers.emplace(binding.ticker, true).second) {
        return std::unexpected(KalshiAdapterCreateError::kInvalidConfig);
      }
    }
    auto http = kalshi_internal::SignedHttpClient::Create(std::move(config.base_url), std::move(config.api_key_id),
                                                          std::move(config.private_key_pem), config.request_timeout);
    if (!http) {
      switch (http.error()) {
        case kalshi_internal::SignedHttpClientCreateError::kInvalidConfig:
          return std::unexpected(KalshiAdapterCreateError::kInvalidConfig);
        case kalshi_internal::SignedHttpClientCreateError::kInvalidPrivateKey:
          return std::unexpected(KalshiAdapterCreateError::kInvalidPrivateKey);
        case kalshi_internal::SignedHttpClientCreateError::kTransportInitializationFailed:
          return std::unexpected(KalshiAdapterCreateError::kTransportInitializationFailed);
      }
      return std::unexpected(KalshiAdapterCreateError::kInvalidConfig);
    }
    auto impl = std::make_unique<Impl>(std::move(config), std::move(*http));
    return std::unique_ptr<KalshiAdapter>(new KalshiAdapter(std::move(impl)));
  } catch (...) {
    return std::unexpected(KalshiAdapterCreateError::kInvalidConfig);
  }
}

VenueSubmitResult KalshiAdapter::Submit(const risk::OrderIntent& order) noexcept {
  try {
    if (Preflight(order) != VenuePreflightResult::kSupported) {
      return {.outcome = SubmitOutcome::kRejected, .venue_order_id = std::nullopt};
    }
    const auto* ticker = impl_->Ticker(order.market_id);
    Json::Value payload(Json::objectValue);
    payload["ticker"] = *ticker;
    payload["client_order_id"] = ClientOrderUuid(order.client_order_id);
    payload["side"] = "bid";
    payload["count"] = FormatFixed(order.quantity.microunits, 10'000, 2);
    payload["price"] = FormatFixed(order.price.micros_per_unit, 100, 4);
    payload["time_in_force"] = "good_till_canceled";
    payload["self_trade_prevention_type"] = "taker_at_cross";
    payload["subaccount"] = impl_->subaccount_;
    const auto response = impl_->http_->Request("POST", "/portfolio/events/orders", {}, JsonText(payload));
    if (!response.transport_ok) {
      return {.outcome = SubmitOutcome::kUnknown, .venue_order_id = std::nullopt};
    }
    if (response.status == 400 || response.status == 401) {
      return {.outcome = SubmitOutcome::kRejected, .venue_order_id = std::nullopt};
    }
    if (response.status != 201) {
      return {.outcome = SubmitOutcome::kUnknown, .venue_order_id = std::nullopt};
    }
    const auto json = ParseJson(response.body);
    if (!json || !(*json)["order_id"].isString() || !(*json)["remaining_count"].isString() || !(*json)["fill_count"].isString()) {
      return {.outcome = SubmitOutcome::kUnknown, .venue_order_id = std::nullopt};
    }
    const auto order_id = (*json)["order_id"].asString();
    if ((*json).isMember("client_order_id") &&
        (!(*json)["client_order_id"].isString() || (*json)["client_order_id"].asString() != ClientOrderUuid(order.client_order_id))) {
      return {.outcome = SubmitOutcome::kUnknown, .venue_order_id = std::nullopt};
    }
    const auto remaining = ParseFixedMicros((*json)["remaining_count"].asString());
    const auto filled = ParseFixedMicros((*json)["fill_count"].asString());
    if (order_id.empty() || order_id.size() > risk::kMaxVenueOrderIdBytes || !remaining || !filled || *remaining < 0 || *filled < 0) {
      return {.outcome = SubmitOutcome::kUnknown, .venue_order_id = std::nullopt};
    }
    return {.outcome = *remaining == 0 && *filled > 0 ? SubmitOutcome::kFilled : SubmitOutcome::kOpen, .venue_order_id = order_id};
  } catch (...) {
    return {.outcome = SubmitOutcome::kUnknown, .venue_order_id = std::nullopt};
  }
}

VenueOrderQueryResult KalshiAdapter::Query(const VenueOrderLookupKey& key) noexcept {
  try {
    const auto* ticker = impl_->Ticker(key.identity.market_id);
    if (ticker == nullptr) {
      return std::unexpected(VenueOrderQueryError::kInvalidResponse);
    }
    // Kalshi has no authoritative GET-by-client_order_id endpoint. Absence
    // from the paginated list is not proof of non-acceptance, so no ack means
    // startup must remain fail-closed.
    if (!key.venue_order_id) {
      return std::unexpected(VenueOrderQueryError::kUnavailable);
    }
    const auto& order_id = *key.venue_order_id;
    if (order_id.empty() || order_id.size() > risk::kMaxVenueOrderIdBytes ||
        order_id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != std::string::npos) {
      return std::unexpected(VenueOrderQueryError::kInvalidResponse);
    }
    const auto response = impl_->http_->Request("GET", "/portfolio/orders/" + order_id);
    if (!response.transport_ok || response.status == 429 || response.status >= 500) {
      return std::unexpected(VenueOrderQueryError::kUnavailable);
    }
    if (response.status != 200) {
      return std::unexpected(response.status == 401 ? VenueOrderQueryError::kUnavailable : VenueOrderQueryError::kInvalidResponse);
    }
    const auto json = ParseJson(response.body);
    if (!json || !(*json)["order"].isObject()) {
      return std::unexpected(VenueOrderQueryError::kInvalidResponse);
    }
    const auto& order = (*json)["order"];
    if (!order["order_id"].isString() || !order["client_order_id"].isString() || !order["ticker"].isString() ||
        !order["status"].isString() || !order["subaccount_number"].isUInt() || !order["book_side"].isString() ||
        !order["yes_price_dollars"].isString() || !order["initial_count_fp"].isString() || !order["fill_count_fp"].isString() ||
        order["order_id"].asString() != order_id || order["client_order_id"].asString() != ClientOrderUuid(key.identity.client_order_id) ||
        order["ticker"].asString() != *ticker) {
      return std::unexpected(VenueOrderQueryError::kInvalidResponse);
    }
    if (order["subaccount_number"].asUInt() != impl_->subaccount_ || order["book_side"].asString() != "bid" ||
        key.identity.side != risk::Side::kBuy) {
      return std::unexpected(VenueOrderQueryError::kInvalidResponse);
    }
    const auto state = DecodeOrderState(order["status"].asString());
    if (!state) {
      return std::unexpected(VenueOrderQueryError::kInvalidResponse);
    }
    const auto venue_price = ParseFixedMicros(order["yes_price_dollars"].asString());
    const auto initial_count = ParseFixedMicros(order["initial_count_fp"].asString());
    const auto fill_count = ParseFixedMicros(order["fill_count_fp"].asString());
    if (!venue_price || !initial_count || !fill_count || *venue_price != key.identity.price.micros_per_unit ||
        *initial_count != key.identity.quantity.microunits || *fill_count < 0 || *fill_count > *initial_count) {
      return std::unexpected(VenueOrderQueryError::kInvalidResponse);
    }
    if (*state == VenueOrderState::kCancelled && *fill_count != 0) {
      // V0 cannot account for the exposure created by a partially filled
      // order whose remainder was cancelled. Keep startup fail-closed.
      return std::unexpected(VenueOrderQueryError::kUnavailable);
    }
    if (*state == VenueOrderState::kRejected && *fill_count != 0) {
      return std::unexpected(VenueOrderQueryError::kInvalidResponse);
    }
    return std::optional<VenueOrderObservation>{VenueOrderObservation{.state = *state, .venue_order_id = order_id}};
  } catch (...) {
    return std::unexpected(VenueOrderQueryError::kUnavailable);
  }
}

bool KalshiAdapter::RefreshMarketData(risk::MarketId market_id) noexcept {
  try {
    impl_->market_data_cursors_.erase(market_id);
    const auto* ticker = impl_->Ticker(market_id);
    if (ticker == nullptr) {
      return false;
    }
    const auto response = impl_->http_->Request("GET", "/markets/" + *ticker + "/orderbook", "?depth=1");
    if (!response.transport_ok || response.status != 200) {
      return false;
    }
    const auto json = ParseJson(response.body);
    if (!json || !(*json)["orderbook_fp"].isObject() || !ValidateOrderbookSide((*json)["orderbook_fp"]["yes_dollars"]) ||
        !ValidateOrderbookSide((*json)["orderbook_fp"]["no_dollars"])) {
      return false;
    }
    impl_->market_data_cursors_.emplace(market_id, MarketDataCursor{
                                                       .market_id = market_id,
                                                       .version = impl_->next_market_data_version_++,
                                                       .received_at = risk::MonotonicClock::now(),
                                                   });
    return true;
  } catch (...) {
    impl_->market_data_cursors_.erase(market_id);
    return false;
  }
}

std::optional<risk::Money> KalshiAdapter::ObserveCurrentEquity() noexcept {
  try {
    const auto balance_query = "?subaccount=" + std::to_string(impl_->subaccount_);
    const auto balance_response = impl_->http_->Request("GET", "/portfolio/balance", balance_query);
    if (!balance_response.transport_ok || balance_response.status != 200) {
      return std::nullopt;
    }
    const auto balance = ParseJson(balance_response.body);
    if (!balance || !(*balance)["balance"].isInt64() || !(*balance)["portfolio_value"].isInt64()) {
      return std::nullopt;
    }
    // balance remains mandatory schema evidence, but portfolio_value is the
    // total account equity: available balance plus current position value.
    const auto portfolio_value_cents = (*balance)["portfolio_value"].asInt64();
    if (portfolio_value_cents < 0 || portfolio_value_cents > std::numeric_limits<std::int64_t>::max() / risk::kMicrosPerCent) {
      return std::nullopt;
    }
    return risk::Money{.micros = portfolio_value_cents * risk::kMicrosPerCent};
  } catch (...) {
    return std::nullopt;
  }
}

bool KalshiAdapter::RefreshAccountState(const TradingDayContext& context) noexcept {
  try {
    impl_->InvalidateAccountState();
    if (context.utc_day < 0 || context.baseline_equity.micros < 0) {
      return false;
    }
    if (impl_->last_published_context_ &&
        (context.utc_day < impl_->last_published_context_->utc_day ||
         (context.utc_day == impl_->last_published_context_->utc_day && context != *impl_->last_published_context_))) {
      return false;
    }
    const auto suffix = "?limit=1000&subaccount=" + std::to_string(impl_->subaccount_);
    const auto positions_response = impl_->http_->Request("GET", "/portfolio/positions", suffix);
    if (!positions_response.transport_ok || positions_response.status != 200) {
      return false;
    }
    const auto positions = ParseJson(positions_response.body);
    if (!positions || !(*positions)["market_positions"].isArray() || !(*positions)["cursor"].isString() ||
        !(*positions)["cursor"].asString().empty()) {
      return false;
    }
    std::unordered_map<risk::MarketId, std::int64_t> market_exposure;
    std::int64_t total_exposure{};
    for (const auto& position : (*positions)["market_positions"]) {
      if (!position["ticker"].isString() || !position["market_exposure_dollars"].isString()) {
        return false;
      }
      const auto exposure = ParseFixedMicros(position["market_exposure_dollars"].asString());
      if (!exposure || *exposure == std::numeric_limits<std::int64_t>::min()) {
        return false;
      }
      const auto gross = *exposure < 0 ? -*exposure : *exposure;
      const auto next_total = CheckedAdd(total_exposure, gross);
      if (!next_total) {
        return false;
      }
      total_exposure = *next_total;
      const auto market = impl_->market_ids_.find(position["ticker"].asString());
      if (market != impl_->market_ids_.end() && !market_exposure.emplace(market->second, gross).second) {
        return false;
      }
    }
    const auto equity = ObserveCurrentEquity();
    if (!equity) {
      return false;
    }
    const auto pnl = CheckedAdd(equity->micros, -context.baseline_equity.micros);
    if (!pnl) {
      return false;
    }
    const auto now = risk::MonotonicClock::now();
    const AccountStateVersion version{.value = impl_->next_version_++};
    for (const auto& [market_id, ticker] : impl_->tickers_) {
      static_cast<void>(ticker);
      impl_->snapshots_.emplace(market_id, AccountStateSnapshot{
                                               .state =
                                                   risk::AccountState{
                                                       .market_gross_exposure = risk::Money{market_exposure[market_id]},
                                                       .total_gross_exposure = risk::Money{total_exposure},
                                                       .pnl_since_baseline = risk::Money{*pnl},
                                                   },
                                               .version = version,
                                               .received_at = now,
                                           });
    }
    impl_->cursor_ = AccountStateCursor{.version = version, .received_at = now};
    impl_->last_published_context_ = context;
    return true;
  } catch (...) {
    impl_->InvalidateAccountState();
    return false;
  }
}

std::optional<AccountStateCursor> KalshiAdapter::CurrentCursor() const noexcept {
  return impl_->cursor_;
}

std::optional<AccountStateSnapshot> KalshiAdapter::Snapshot(risk::MarketId market_id) const noexcept {
  const auto iterator = impl_->snapshots_.find(market_id);
  return iterator == impl_->snapshots_.end() ? std::nullopt : std::optional<AccountStateSnapshot>{iterator->second};
}

std::optional<MarketDataCursor> KalshiAdapter::Current(risk::MarketId market_id) const noexcept {
  const auto iterator = impl_->market_data_cursors_.find(market_id);
  return iterator == impl_->market_data_cursors_.end() ? std::nullopt : std::optional<MarketDataCursor>{iterator->second};
}

}  // namespace botguard::execution
