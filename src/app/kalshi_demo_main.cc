#include "execution/kalshi_adapter.h"
#include "execution/kalshi_startup_reconciler.h"
#include "execution/order_submission_coordinator.h"
#include "execution/runtime_lease.h"
#include "execution/sqlite_order_state_store.h"
#include "execution/sqlite_runtime_state_store.h"
#include "execution/sqlite_storage_pair.h"
#include "risk/risk_engine.h"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace std::chrono_literals;

namespace execution = botguard::execution;
namespace risk = botguard::risk;

constexpr std::string_view kDemoBaseUrl = "https://external-api.demo.kalshi.co/trade-api/v2";

struct CommonArguments {
  std::filesystem::path database_path;
  std::string ticker;
  risk::MarketId market_id{};
  std::uint32_t subaccount{};
};

struct SubmitArguments {
  risk::ClientOrderId client_order_id{};
  risk::Price price{};
  risk::Quantity quantity{};
};

void Usage(const char* program) {
  std::cerr << "Usage:\n"
            << "  " << program << " status DB_PATH TICKER MARKET_ID SUBACCOUNT\n"
            << "  " << program
            << " submit DB_PATH TICKER MARKET_ID SUBACCOUNT CLIENT_ORDER_ID PRICE_CENTS"
               " QUANTITY_CENTICONTRACTS\n\n"
            << "Required environment:\n"
            << "  KALSHI_DEMO_API_KEY_ID\n"
            << "  KALSHI_DEMO_PRIVATE_KEY_PATH\n\n"
            << "SUBACCOUNT must be a dedicated numbered Demo subaccount in the range 1..32.\n"
            << "Submit also requires BOTGUARD_DEMO_CONFIRM_SUBMIT=YES.\n";
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> ParseInteger(std::string_view text) noexcept {
  Integer value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] const char* Environment(const char* name) noexcept {
  const auto* value = std::getenv(name);
  return value != nullptr && *value != '\0' ? value : nullptr;
}

[[nodiscard]] std::optional<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad() || contents.empty()) {
    return std::nullopt;
  }
  return contents;
}

[[nodiscard]] std::string RuntimeBindingFingerprint(std::string_view api_key_id, std::uint32_t subaccount, risk::MarketId market_id,
                                                    std::string_view ticker) {
  const auto identity = "venue=kalshi-demo\nbase_url=" + std::string(kDemoBaseUrl) + "\napi_key_id=" + std::string(api_key_id) +
                        "\nsubaccount=" + std::to_string(subaccount) + "\nmarket=" + std::to_string(market_id) + ":" + std::string(ticker) +
                        "\n";
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char*>(identity.data()), identity.size(), digest.data());
  constexpr std::string_view hex = "0123456789abcdef";
  std::string fingerprint(SHA256_DIGEST_LENGTH * 2, '0');
  for (std::size_t index = 0; index < digest.size(); ++index) {
    fingerprint[index * 2] = hex[digest[index] >> 4U];
    fingerprint[index * 2 + 1] = hex[digest[index] & 0x0FU];
  }
  return fingerprint;
}

[[nodiscard]] std::optional<execution::StorageGeneration> GenerateStorageGeneration() {
  std::array<unsigned char, 32> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    return std::nullopt;
  }
  constexpr std::string_view hex = "0123456789abcdef";
  std::string value(random.size() * 2, '0');
  for (std::size_t index = 0; index < random.size(); ++index) {
    value[index * 2] = hex[random[index] >> 4U];
    value[index * 2 + 1] = hex[random[index] & 0x0FU];
  }
  return execution::StorageGeneration{.value = std::move(value)};
}

[[nodiscard]] std::optional<CommonArguments> ParseCommon(char** argv) {
  const auto market_id = ParseInteger<risk::MarketId>(argv[4]);
  const auto subaccount = ParseInteger<std::uint32_t>(argv[5]);
  if (!market_id || *market_id > static_cast<risk::MarketId>(std::numeric_limits<std::int64_t>::max()) || !subaccount || *subaccount == 0 ||
      *subaccount > 32 || std::string_view(argv[2]).empty() || std::string_view(argv[3]).empty()) {
    return std::nullopt;
  }
  return CommonArguments{
      .database_path = argv[2],
      .ticker = argv[3],
      .market_id = *market_id,
      .subaccount = *subaccount,
  };
}

[[nodiscard]] std::optional<SubmitArguments> ParseSubmit(char** argv) noexcept {
  const auto client_order_id = ParseInteger<risk::ClientOrderId>(argv[6]);
  const auto price_cents = ParseInteger<std::int64_t>(argv[7]);
  const auto quantity_centicontracts = ParseInteger<std::int64_t>(argv[8]);
  if (!client_order_id || *client_order_id > static_cast<risk::ClientOrderId>(std::numeric_limits<std::int64_t>::max()) || !price_cents ||
      *price_cents <= 0 || *price_cents >= risk::kCentsPerUsd || !quantity_centicontracts || *quantity_centicontracts <= 0 ||
      *quantity_centicontracts > std::numeric_limits<std::int64_t>::max() / 10'000) {
    return std::nullopt;
  }
  return SubmitArguments{
      .client_order_id = *client_order_id,
      .price = risk::Price::FromCents(*price_cents),
      .quantity = risk::Quantity{.microunits = *quantity_centicontracts * 10'000},
  };
}

[[nodiscard]] const char* ToString(execution::SubmitOutcome outcome) noexcept {
  switch (outcome) {
    case execution::SubmitOutcome::kOpen:
      return "OPEN";
    case execution::SubmitOutcome::kFilled:
      return "FILLED";
    case execution::SubmitOutcome::kRejected:
      return "REJECTED";
    case execution::SubmitOutcome::kUnknown:
      return "UNKNOWN";
  }
  return "INVALID";
}

[[nodiscard]] const char* ToString(execution::SubmissionStatus status) noexcept {
  switch (status) {
    case execution::SubmissionStatus::kRiskRejected:
      return "RISK_REJECTED";
    case execution::SubmissionStatus::kVenuePreflightRejected:
      return "VENUE_PREFLIGHT_REJECTED";
    case execution::SubmissionStatus::kPreSubmitPersistenceFailed:
      return "PRE_SUBMIT_PERSISTENCE_FAILED";
    case execution::SubmissionStatus::kAbortedBeforeSubmit:
      return "ABORTED_BEFORE_SUBMIT";
    case execution::SubmissionStatus::kPreSubmitAbortPersistenceFailed:
      return "PRE_SUBMIT_ABORT_PERSISTENCE_FAILED";
    case execution::SubmissionStatus::kSubmitted:
      return "SUBMITTED";
    case execution::SubmissionStatus::kPostSubmitPersistenceFailed:
      return "POST_SUBMIT_PERSISTENCE_FAILED";
    case execution::SubmissionStatus::kRegistryTransitionFailed:
      return "REGISTRY_TRANSITION_FAILED";
  }
  return "INVALID";
}

void PrintRegistry(const risk::OrderRegistry& registry) {
  const auto entries = registry.Snapshot();
  std::cout << "durable_orders=" << entries.size() << '\n';
  for (const auto& entry : entries) {
    std::cout << "  client_order_id=" << entry.client_order_id << " state=" << risk::ToString(entry.state);
    if (entry.venue_order_id) {
      std::cout << " venue_order_id=" << *entry.venue_order_id;
    }
    std::cout << '\n';
  }
}

[[nodiscard]] risk::RiskLimits DemoLimits() noexcept {
  return risk::RiskLimits{
      .max_order_notional = risk::Money::FromWholeUsd(10),
      .max_market_gross_exposure = risk::Money::FromWholeUsd(100),
      .max_total_gross_exposure = risk::Money::FromWholeUsd(250),
      .max_loss_since_baseline = risk::Money::FromWholeUsd(25),
      .max_market_data_age = 5s,
  };
}

}  // namespace

int main(int argc, char** argv) {
  const bool status_mode = argc == 6 && std::string_view(argv[1]) == "status";
  const bool submit_mode = argc == 9 && std::string_view(argv[1]) == "submit";
  if (!status_mode && !submit_mode) {
    Usage(argv[0]);
    return 2;
  }

  const auto arguments = ParseCommon(argv);
  const auto submit_arguments = submit_mode ? ParseSubmit(argv) : std::optional<SubmitArguments>{};
  if (!arguments || (submit_mode && !submit_arguments)) {
    std::cerr << "invalid command arguments\n";
    Usage(argv[0]);
    return 2;
  }

  if (submit_mode) {
    const auto* confirmation = Environment("BOTGUARD_DEMO_CONFIRM_SUBMIT");
    if (confirmation == nullptr || std::string_view(confirmation) != "YES") {
      std::cerr << "submit disabled: set BOTGUARD_DEMO_CONFIRM_SUBMIT=YES explicitly\n";
      return 2;
    }
  }

  const auto* api_key_id = Environment("KALSHI_DEMO_API_KEY_ID");
  const auto* private_key_path = Environment("KALSHI_DEMO_PRIVATE_KEY_PATH");
  if (api_key_id == nullptr || private_key_path == nullptr) {
    std::cerr << "missing KALSHI_DEMO_API_KEY_ID or KALSHI_DEMO_PRIVATE_KEY_PATH\n";
    return 2;
  }
  const auto private_key = ReadFile(private_key_path);
  if (!private_key) {
    std::cerr << "cannot read a non-empty private key file\n";
    return 2;
  }

  auto runtime_lease = execution::RuntimeLease::Acquire(arguments->database_path);
  if (!runtime_lease) {
    std::cerr << "exclusive runtime lease acquisition failed, error=" << static_cast<int>(runtime_lease.error()) << '\n';
    return 1;
  }

  auto runtime_state_path = arguments->database_path;
  runtime_state_path += ".runtime";
  const execution::RuntimeBinding runtime_binding{
      .fingerprint = RuntimeBindingFingerprint(api_key_id, arguments->subaccount, arguments->market_id, arguments->ticker),
  };
  const auto pair_presence = execution::InspectStoragePair(arguments->database_path, runtime_state_path);
  if (!pair_presence) {
    std::cerr << "storage pair inspection failed closed, error=" << static_cast<int>(pair_presence.error()) << '\n';
    return 1;
  }
  const auto new_generation = *pair_presence == execution::StoragePairPresence::kAbsent ? GenerateStorageGeneration()
                                                                                        : std::optional<execution::StorageGeneration>{};
  if (*pair_presence == execution::StoragePairPresence::kAbsent && !new_generation) {
    std::cerr << "secure storage generation creation failed\n";
    return 1;
  }
  auto storage =
      *pair_presence == execution::StoragePairPresence::kAbsent
          ? execution::SqliteStoragePair::CreateNew(arguments->database_path, runtime_state_path, runtime_binding, *new_generation)
          : execution::SqliteStoragePair::OpenExisting(arguments->database_path, runtime_state_path, runtime_binding);
  if (!storage) {
    std::cerr << "SQLite storage pair open failed, error=" << static_cast<int>(storage.error()) << '\n';
    return 1;
  }
  auto& runtime_state = storage->RuntimeState();
  auto& state_store = storage->OrderState();

  auto adapter = execution::KalshiAdapter::Create(execution::KalshiAdapterConfig{
      .base_url = std::string(kDemoBaseUrl),
      .api_key_id = api_key_id,
      .private_key_pem = *private_key,
      .markets = {{.market_id = arguments->market_id, .ticker = arguments->ticker}},
      .subaccount = arguments->subaccount,
      .request_timeout = 5s,
  });
  if (!adapter) {
    std::cerr << "Kalshi adapter creation failed, error=" << static_cast<int>(adapter.error()) << '\n';
    return 1;
  }

  execution::SystemTradingDayProvider trading_day_provider;
  const auto trading_day_utc = trading_day_provider.CurrentUtcDay();
  const auto stored_context = runtime_state.LoadTradingDayContext();
  if (!trading_day_utc || *trading_day_utc < 0 || !stored_context || (*stored_context && (*stored_context)->utc_day > *trading_day_utc)) {
    std::cerr << "trading-day context load failed closed\n";
    return 1;
  }
  auto trading_day_context = *stored_context;
  if (!trading_day_context || trading_day_context->utc_day < *trading_day_utc) {
    const auto authoritative_equity = (*adapter)->ObserveCurrentEquity();
    if (!authoritative_equity) {
      std::cerr << "authoritative baseline-equity observation failed closed\n";
      return 1;
    }
    const auto baseline_equity = runtime_state.ResolveDailyBaselineEquity(*trading_day_utc, *authoritative_equity);
    if (!baseline_equity) {
      std::cerr << "per-day equity baseline failed closed, error=" << static_cast<int>(baseline_equity.error()) << '\n';
      return 1;
    }
    trading_day_context = execution::TradingDayContext{.utc_day = *trading_day_utc, .baseline_equity = *baseline_equity};
  }
  std::cout << "trading_day_utc=" << trading_day_context->utc_day
            << " durable_baseline_equity_micros=" << trading_day_context->baseline_equity.micros << '\n';

  risk::OrderRegistry registry;
  execution::KalshiStartupReconciler reconciler(**adapter, *trading_day_context);
  auto recovery = execution::StartupRecoverySession::Recover(registry, state_store, reconciler, **adapter,
                                                             execution::AccountStateSafetyPolicy{.max_age = 10s});
  if (!recovery) {
    std::cerr << "startup recovery failed closed, error=" << static_cast<int>(recovery.error()) << '\n';
    return 1;
  }

  const auto snapshot = (*adapter)->Snapshot(arguments->market_id);
  if (!snapshot) {
    std::cerr << "authoritative market snapshot disappeared\n";
    return 1;
  }
  std::cout << "kalshi_demo_ready ticker=" << arguments->ticker << " market_id=" << arguments->market_id
            << " subaccount=" << arguments->subaccount << " market_exposure_micros=" << snapshot->state.market_gross_exposure.micros
            << " total_exposure_micros=" << snapshot->state.total_gross_exposure.micros
            << " pnl_since_baseline_micros=" << snapshot->state.pnl_since_baseline.micros << '\n';
  PrintRegistry(registry);

  if (status_mode) {
    return 0;
  }

  if (!(*adapter)->RefreshMarketData(arguments->market_id)) {
    std::cerr << "authoritative Kalshi market-data refresh failed; submit remains closed\n";
    return 1;
  }

  risk::RiskEngine risk_engine(DemoLimits());
  execution::OrderSubmissionCoordinator coordinator(risk_engine, std::move(*recovery), **adapter, **adapter, trading_day_provider,
                                                    *trading_day_context);
  const risk::OrderIntent intent{
      .strategy_id = 1,
      .client_order_id = submit_arguments->client_order_id,
      .market_id = arguments->market_id,
      .side = risk::Side::kBuy,
      .price = submit_arguments->price,
      .quantity = submit_arguments->quantity,
      // Ignored by OrderSubmissionCoordinator. Freshness comes only from the
      // adapter's BotGuard-owned orderbook snapshot above.
      .market_data_received_at = {},
  };
  const auto result = coordinator.Execute(intent);
  std::cout << "submission_status=" << ToString(result.status) << " notional_micros=" << result.risk_decision.OrderNotional().micros;
  if (result.submit_outcome) {
    std::cout << " venue_outcome=" << ToString(*result.submit_outcome);
  }
  std::cout << '\n';
  for (const auto reason : result.risk_decision.Reasons()) {
    std::cout << "  reject_reason=" << risk::ToString(reason) << '\n';
  }
  PrintRegistry(registry);

  return result.status == execution::SubmissionStatus::kSubmitted ? 0 : 1;
}
