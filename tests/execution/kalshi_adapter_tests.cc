#include "execution/kalshi_adapter.h"
#include "execution/kalshi_startup_reconciler.h"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace execution = botguard::execution;
namespace risk = botguard::risk;

[[nodiscard]] std::string TestPrivateKey() {
  EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  EXPECT_NE(raw_context, nullptr);
  if (raw_context == nullptr) {
    return {};
  }
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(raw_context, EVP_PKEY_CTX_free);
  EXPECT_EQ(EVP_PKEY_keygen_init(context.get()), 1);
  EXPECT_EQ(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048), 1);
  EVP_PKEY* raw_key = nullptr;
  EXPECT_EQ(EVP_PKEY_keygen(context.get(), &raw_key), 1);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
  BIO* raw_bio = BIO_new(BIO_s_mem());
  EXPECT_NE(raw_bio, nullptr);
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(raw_bio, BIO_free);
  EXPECT_EQ(PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr), 1);
  char* data = nullptr;
  const auto size = BIO_get_mem_data(bio.get(), &data);
  return size > 0 ? std::string(data, static_cast<std::size_t>(size)) : std::string{};
}

[[nodiscard]] std::string ClientOrderUuid(risk::ClientOrderId id) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  const auto input = std::to_string(id);
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data());
  digest[6] = static_cast<unsigned char>((digest[6] & 0x0FU) | 0x50U);
  digest[8] = static_cast<unsigned char>((digest[8] & 0x3FU) | 0x80U);
  std::array<char, 37> output{};
  std::snprintf(output.data(), output.size(), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", digest[0], digest[1],
                digest[2], digest[3], digest[4], digest[5], digest[6], digest[7], digest[8], digest[9], digest[10], digest[11], digest[12],
                digest[13], digest[14], digest[15]);
  return output.data();
}

[[nodiscard]] std::string HeaderValue(const std::string& request, std::string_view name) {
  const auto begin = request.find(std::string(name) + ": ");
  if (begin == std::string::npos) {
    return {};
  }
  const auto value_begin = begin + name.size() + 2;
  const auto end = request.find("\r\n", value_begin);
  return end == std::string::npos ? std::string{} : request.substr(value_begin, end - value_begin);
}

[[nodiscard]] bool VerifySignature(const std::string& private_key_pem, const std::string& request, std::string_view method,
                                   std::string_view path) {
  const auto timestamp = HeaderValue(request, "KALSHI-ACCESS-TIMESTAMP");
  const auto encoded = HeaderValue(request, "KALSHI-ACCESS-SIGNATURE");
  if (timestamp.empty() || encoded.empty()) {
    return false;
  }
  std::vector<unsigned char> signature(3 * ((encoded.size() + 3) / 4));
  const auto decoded =
      EVP_DecodeBlock(signature.data(), reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
  if (decoded < 0) {
    return false;
  }
  auto signature_size = static_cast<std::size_t>(decoded);
  if (!encoded.empty() && encoded.back() == '=') {
    --signature_size;
  }
  if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') {
    --signature_size;
  }
  BIO* raw_bio = BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size()));
  if (raw_bio == nullptr) {
    return false;
  }
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(raw_bio, BIO_free);
  EVP_PKEY* raw_key = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
  if (raw_key == nullptr) {
    return false;
  }
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
  EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
  if (raw_context == nullptr) {
    return false;
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw_context, EVP_MD_CTX_free);
  EVP_PKEY_CTX* key_context = nullptr;
  const auto message = timestamp + std::string(method) + std::string(path);
  return EVP_DigestVerifyInit(context.get(), &key_context, EVP_sha256(), nullptr, key.get()) == 1 && key_context != nullptr &&
         EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) > 0 &&
         EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_DIGEST) > 0 &&
         EVP_DigestVerifyUpdate(context.get(), message.data(), message.size()) == 1 &&
         EVP_DigestVerifyFinal(context.get(), signature.data(), signature_size) == 1;
}

class LocalHttpServer {
 public:
  explicit LocalHttpServer(std::vector<std::string> bodies, int status = 200, std::chrono::milliseconds response_delay = {})
      : bodies_(std::move(bodies)), status_(status), response_delay_(response_delay) {
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
      unavailable_errno_ = errno;
      return;
    }
    int reuse = 1;
    EXPECT_EQ(::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)), 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    EXPECT_EQ(::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    EXPECT_EQ(::listen(socket_, 4), 0);
    socklen_t size = sizeof(address);
    EXPECT_EQ(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size), 0);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { Run(); });
  }

  ~LocalHttpServer() {
    if (thread_.joinable()) {
      thread_.join();
    }
    if (socket_ >= 0) {
      ::close(socket_);
    }
  }

  [[nodiscard]] std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port_) + "/trade-api/v2"; }

  [[nodiscard]] bool Available() const noexcept { return socket_ >= 0; }

  [[nodiscard]] int UnavailableErrno() const noexcept { return unavailable_errno_; }

  void Wait() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] const std::vector<std::string>& Requests() const noexcept { return requests_; }

 private:
  void Run() {
    for (const auto& body : bodies_) {
      const int client = ::accept(socket_, nullptr, nullptr);
      if (client < 0) {
        return;
      }
      std::string request;
      std::array<char, 4096> buffer{};
      std::size_t expected_size = std::string::npos;
      while (expected_size == std::string::npos || request.size() < expected_size) {
        const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
          break;
        }
        request.append(buffer.data(), static_cast<std::size_t>(count));
        const auto header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos) {
          std::size_t content_length{};
          const auto header = request.find("Content-Length:");
          if (header != std::string::npos) {
            content_length = static_cast<std::size_t>(std::stoul(request.substr(header + 15)));
          }
          expected_size = header_end + 4 + content_length;
        }
      }
      requests_.push_back(std::move(request));
      std::this_thread::sleep_for(response_delay_);
      const auto response = "HTTP/1.1 " + std::to_string(status_) +
                            " OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                            "\r\nConnection: close\r\n\r\n" + body;
      static_cast<void>(::send(client, response.data(), response.size(), MSG_NOSIGNAL));
      ::close(client);
    }
  }

  int socket_{-1};
  int unavailable_errno_{};
  std::uint16_t port_{};
  std::vector<std::string> bodies_;
  int status_{};
  std::chrono::milliseconds response_delay_{};
  std::vector<std::string> requests_;
  std::thread thread_;
};

[[nodiscard]] execution::KalshiAdapterConfig Config(std::string base_url) {
  return execution::KalshiAdapterConfig{
      .base_url = std::move(base_url),
      .api_key_id = "test-key-id",
      .private_key_pem = TestPrivateKey(),
      .markets = {{.market_id = 10, .ticker = "TEST-MARKET"}},
      .subaccount = 0,
      .request_timeout = std::chrono::seconds(2),
  };
}

[[nodiscard]] execution::TradingDayContext Context(std::int64_t day = 20'000,
                                                   risk::Money baseline_equity = risk::Money::FromWholeUsd(100)) noexcept {
  return execution::TradingDayContext{.utc_day = day, .baseline_equity = baseline_equity};
}

[[nodiscard]] risk::OrderIdentity Identity() noexcept {
  return risk::OrderIdentity{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(1),
  };
}

[[nodiscard]] std::string OrderResponse(std::string_view status, std::string_view price = "0.2500", std::string_view initial_count = "1.00",
                                        std::string_view fill_count = "0.00") {
  return "{\"order\":{\"order_id\":\"venue-order-1\",\"client_order_id\":\"" + ClientOrderUuid(101) +
         "\",\"ticker\":\"TEST-MARKET\",\"status\":\"" + std::string(status) +
         "\",\"book_side\":\"bid\",\"subaccount_number\":0,\"yes_price_dollars\":\"" + std::string(price) + "\",\"initial_count_fp\":\"" +
         std::string(initial_count) + "\",\"fill_count_fp\":\"" + std::string(fill_count) + "\"}}";
}

class MemoryOrderStateStore final : public execution::OrderStateStore {
 public:
  explicit MemoryOrderStateStore(risk::OrderRegistryEntry entry) : entries_{std::move(entry)} {}

  [[nodiscard]] bool PersistTransition(const risk::OrderRegistryEntry& entry, execution::OrderAuditEventType event_type) noexcept override {
    bool replaced{};
    for (auto& current : entries_) {
      if (current.client_order_id == entry.client_order_id) {
        current = entry;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      entries_.push_back(entry);
    }
    events_.push_back(execution::OrderAuditEvent{
        .sequence = events_.size() + 1,
        .client_order_id = entry.client_order_id,
        .type = event_type,
        .resulting_state = entry.state,
        .venue_order_id = entry.venue_order_id,
    });
    return true;
  }

  [[nodiscard]] bool LoadAll(std::vector<risk::OrderRegistryEntry>& entries) noexcept override {
    entries = entries_;
    return true;
  }

  [[nodiscard]] bool LoadAuditEvents(std::vector<execution::OrderAuditEvent>& events) noexcept override {
    events = events_;
    return true;
  }

 private:
  std::vector<risk::OrderRegistryEntry> entries_;
  std::vector<execution::OrderAuditEvent> events_;
};

[[nodiscard]] risk::OrderRegistryEntry UnknownEntry() {
  return risk::OrderRegistryEntry{
      .client_order_id = 101,
      .state = risk::OrderState::kUnknown,
      .market_id = 10,
      .reserved_notional = risk::Money{.micros = 250'000},
      .reservation_active = true,
      .identity = Identity(),
      .venue_order_id = std::string{"venue-order-1"},
  };
}

TEST(KalshiAdapterTest, QueryWithoutVenueAcknowledgementFailsClosedWithoutHttpRequest) {
  auto config = Config("http://127.0.0.1:1/trade-api/v2");
  auto adapter = execution::KalshiAdapter::Create(std::move(config));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Query(execution::VenueOrderLookupKey{
      .identity = risk::OrderIdentity{.strategy_id = 1,
                                      .client_order_id = 101,
                                      .market_id = 10,
                                      .side = risk::Side::kBuy,
                                      .price = risk::Price::FromCents(25),
                                      .quantity = risk::Quantity::FromWhole(1)},
      .venue_order_id = std::nullopt,
  });

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), execution::VenueOrderQueryError::kUnavailable);
}

TEST(KalshiAdapterTest, SellIsRejectedBeforeHttpBecauseV0CannotReserveOppositeOutcomeLiability) {
  auto adapter = execution::KalshiAdapter::Create(Config("http://127.0.0.1:1/trade-api/v2"));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Submit(risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kSell,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(100),
      .market_data_received_at = risk::MonotonicClock::now(),
  });

  EXPECT_EQ(result.outcome, execution::SubmitOutcome::kRejected);
  EXPECT_FALSE(result.venue_order_id.has_value());
}

TEST(KalshiAdapterTest, PreflightChecksV0RepresentabilityWithoutHttp) {
  auto adapter = execution::KalshiAdapter::Create(Config("http://127.0.0.1:1/trade-api/v2"));
  ASSERT_TRUE(adapter.has_value());

  risk::OrderIntent order{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(1),
      .market_data_received_at = {},
  };
  EXPECT_EQ((*adapter)->Preflight(order), execution::VenuePreflightResult::kSupported);

  order.side = risk::Side::kSell;
  EXPECT_EQ((*adapter)->Preflight(order), execution::VenuePreflightResult::kUnsupported);
  order.side = risk::Side::kBuy;

  order.market_id = 11;
  EXPECT_EQ((*adapter)->Preflight(order), execution::VenuePreflightResult::kUnsupported);
  order.market_id = 10;

  order.price = risk::Price{.micros_per_unit = 1};
  EXPECT_EQ((*adapter)->Preflight(order), execution::VenuePreflightResult::kUnsupported);
  order.price = risk::Price::FromCents(25);

  order.quantity = risk::Quantity{.microunits = 1};
  EXPECT_EQ((*adapter)->Preflight(order), execution::VenuePreflightResult::kUnsupported);
}

TEST(KalshiAdapterTest, MalformedSuccessfulSubmitIsUnknown) {
  LocalHttpServer server({R"({"unexpected":true})"}, 201);
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Submit(risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(1),
      .market_data_received_at = risk::MonotonicClock::now(),
  });

  EXPECT_EQ(result.outcome, execution::SubmitOutcome::kUnknown);
}

TEST(KalshiAdapterTest, DuplicateSubmitConflictIsUnknown) {
  LocalHttpServer server({R"({"code":"duplicate_client_order_id"})"}, 409);
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Submit(risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(1),
      .market_data_received_at = risk::MonotonicClock::now(),
  });

  EXPECT_EQ(result.outcome, execution::SubmitOutcome::kUnknown);
}

TEST(KalshiAdapterTest, UnexpectedClientErrorIsUnknown) {
  LocalHttpServer server({R"({"error":"request timeout"})"}, 408);
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());
  const auto result = (*adapter)->Submit(risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(1),
      .market_data_received_at = risk::MonotonicClock::now(),
  });
  EXPECT_EQ(result.outcome, execution::SubmitOutcome::kUnknown);
}

TEST(KalshiAdapterTest, DocumentedValidationAndAuthenticationErrorsAreRejected) {
  for (const int status : {400, 401}) {
    LocalHttpServer server({R"({"error":"definitive rejection"})"}, status);
    if (!server.Available()) {
      GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
    }
    auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
    ASSERT_TRUE(adapter.has_value());
    const auto result = (*adapter)->Submit(risk::OrderIntent{
        .strategy_id = 1,
        .client_order_id = 101,
        .market_id = 10,
        .side = risk::Side::kBuy,
        .price = risk::Price::FromCents(25),
        .quantity = risk::Quantity::FromWhole(1),
        .market_data_received_at = risk::MonotonicClock::now(),
    });
    EXPECT_EQ(result.outcome, execution::SubmitOutcome::kRejected);
  }
}

TEST(KalshiAdapterTest, WrongEchoedClientOrderIdMakesSuccessfulSubmitUnknown) {
  LocalHttpServer server(
      {R"({"order_id":"venue-order-1","client_order_id":"wrong","fill_count":"0.00","remaining_count":"1.00","ts_ms":1})"}, 201);
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());
  const auto result = (*adapter)->Submit(risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(1),
      .market_data_received_at = risk::MonotonicClock::now(),
  });
  EXPECT_EQ(result.outcome, execution::SubmitOutcome::kUnknown);
  EXPECT_FALSE(result.venue_order_id.has_value());
}

TEST(KalshiAdapterTest, RateLimitAndServerFailureAreUnknown) {
  for (const int status : {429, 500}) {
    LocalHttpServer server({R"({"error":"try later"})"}, status);
    if (!server.Available()) {
      GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
    }
    auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
    ASSERT_TRUE(adapter.has_value());
    const auto result = (*adapter)->Submit(risk::OrderIntent{
        .strategy_id = 1,
        .client_order_id = 101,
        .market_id = 10,
        .side = risk::Side::kBuy,
        .price = risk::Price::FromCents(25),
        .quantity = risk::Quantity::FromWhole(1),
        .market_data_received_at = risk::MonotonicClock::now(),
    });
    EXPECT_EQ(result.outcome, execution::SubmitOutcome::kUnknown);
  }
}

TEST(KalshiAdapterTest, TransportTimeoutIsUnknown) {
  LocalHttpServer server({R"({"order_id":"too-late"})"}, 201, std::chrono::milliseconds(200));
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto config = Config(server.BaseUrl());
  config.request_timeout = std::chrono::milliseconds(50);
  auto adapter = execution::KalshiAdapter::Create(std::move(config));
  ASSERT_TRUE(adapter.has_value());
  const auto result = (*adapter)->Submit(risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(1),
      .market_data_received_at = risk::MonotonicClock::now(),
  });
  EXPECT_EQ(result.outcome, execution::SubmitOutcome::kUnknown);
}

TEST(KalshiAdapterTest, SubmitsLimitOrderOverSignedHttpAndReturnsAcknowledgement) {
  const auto body = "{\"order_id\":\"venue-order-1\",\"client_order_id\":\"" + ClientOrderUuid(101) +
                    "\",\"fill_count\":\"0.00\",\"remaining_count\":\"1.00\",\"ts_ms\":1}";
  LocalHttpServer server({body}, 201);
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto config = Config(server.BaseUrl());
  const auto private_key_pem = config.private_key_pem;
  auto adapter = execution::KalshiAdapter::Create(std::move(config));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Submit(risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = 101,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(1),
      .market_data_received_at = risk::MonotonicClock::now(),
  });
  server.Wait();

  EXPECT_EQ(result.outcome, execution::SubmitOutcome::kOpen);
  EXPECT_EQ(result.venue_order_id, "venue-order-1");
  ASSERT_EQ(server.Requests().size(), 1U);
  EXPECT_NE(server.Requests()[0].find("POST /trade-api/v2/portfolio/events/orders "), std::string::npos);
  EXPECT_NE(server.Requests()[0].find("KALSHI-ACCESS-SIGNATURE:"), std::string::npos);
  EXPECT_NE(server.Requests()[0].find("\"ticker\":\"TEST-MARKET\""), std::string::npos);
  EXPECT_NE(server.Requests()[0].find("\"price\":\"0.2500\""), std::string::npos);
  EXPECT_NE(server.Requests()[0].find("\"count\":\"1.00\""), std::string::npos);
  EXPECT_TRUE(VerifySignature(private_key_pem, server.Requests()[0], "POST", "/trade-api/v2/portfolio/events/orders"));
}

TEST(KalshiAdapterTest, RefreshesTrustedMarketDataFreshnessFromOrderbook) {
  LocalHttpServer server({R"({"orderbook_fp":{"yes_dollars":[["0.2500","10.00"]],"no_dollars":[]}})"});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());
  EXPECT_FALSE((*adapter)->Current(10).has_value());

  ASSERT_TRUE((*adapter)->RefreshMarketData(10));
  const auto cursor = (*adapter)->Current(10);

  ASSERT_TRUE(cursor.has_value());
  EXPECT_EQ(cursor->market_id, 10U);
  EXPECT_NE(cursor->version, 0U);
  server.Wait();
  ASSERT_EQ(server.Requests().size(), 1U);
  EXPECT_NE(server.Requests()[0].find("GET /trade-api/v2/markets/TEST-MARKET/orderbook?depth=1 HTTP"), std::string::npos);
}

TEST(KalshiAdapterTest, MalformedOrderbookLevelDoesNotCreateFreshnessEvidence) {
  LocalHttpServer server({R"({"orderbook_fp":{"yes_dollars":[["garbage"]],"no_dollars":[]}})"});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  EXPECT_FALSE((*adapter)->RefreshMarketData(10));
  EXPECT_FALSE((*adapter)->Current(10).has_value());
}

TEST(KalshiAdapterTest, ContextualRefreshComputesPnlSinceDurableBaseline) {
  LocalHttpServer server({
      R"({"market_positions":[{"ticker":"TEST-MARKET","market_exposure_dollars":"12.500000"}],"event_positions":[],"cursor":""})",
      R"({"balance":8000,"portfolio_value":11000,"updated_ts":1})",
  });
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  ASSERT_TRUE((*adapter)->RefreshAccountState(Context()));
  server.Wait();

  const auto snapshot = (*adapter)->Snapshot(10);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->state.market_gross_exposure, risk::Money{.micros = 12'500'000});
  EXPECT_EQ(snapshot->state.total_gross_exposure, risk::Money{.micros = 12'500'000});
  EXPECT_EQ(snapshot->state.pnl_since_baseline, risk::Money::FromWholeUsd(10));
  ASSERT_EQ(server.Requests().size(), 2U);
  EXPECT_NE(server.Requests()[0].find("GET /trade-api/v2/portfolio/positions?"), std::string::npos);
  EXPECT_NE(server.Requests()[0].find("KALSHI-ACCESS-SIGNATURE:"), std::string::npos);
}

TEST(KalshiAdapterTest, ObserveCurrentEquityDoesNotPublishAccountSnapshot) {
  LocalHttpServer server({R"({"balance":8000,"portfolio_value":11000})"});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  const auto equity = (*adapter)->ObserveCurrentEquity();
  server.Wait();

  ASSERT_TRUE(equity.has_value());
  EXPECT_EQ(*equity, risk::Money::FromWholeUsd(110));
  EXPECT_FALSE((*adapter)->CurrentCursor().has_value());
  EXPECT_FALSE((*adapter)->Snapshot(10).has_value());
  ASSERT_EQ(server.Requests().size(), 1U);
  EXPECT_NE(server.Requests()[0].find("GET /trade-api/v2/portfolio/balance?"), std::string::npos);
}

TEST(KalshiAdapterTest, RefreshAccountStateRequiresValidTradingDayContext) {
  auto adapter = execution::KalshiAdapter::Create(Config("http://127.0.0.1:1/trade-api/v2"));
  ASSERT_TRUE(adapter.has_value());

  EXPECT_FALSE((*adapter)->RefreshAccountState(Context(-1)));
  EXPECT_FALSE((*adapter)->RefreshAccountState(Context(20'000, risk::Money{.micros = -1})));
  EXPECT_FALSE((*adapter)->CurrentCursor().has_value());
  EXPECT_FALSE((*adapter)->Snapshot(10).has_value());
}

TEST(KalshiAdapterTest, SameDayDifferentContextIsRejectedAndInvalidatesSnapshot) {
  LocalHttpServer server({
      R"({"market_positions":[],"cursor":""})",
      R"({"balance":10000,"portfolio_value":10000})",
  });
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());
  ASSERT_TRUE((*adapter)->RefreshAccountState(Context()));
  ASSERT_TRUE((*adapter)->Snapshot(10).has_value());

  EXPECT_FALSE((*adapter)->RefreshAccountState(Context(20'000, risk::Money::FromWholeUsd(99))));
  EXPECT_FALSE((*adapter)->CurrentCursor().has_value());
  EXPECT_FALSE((*adapter)->Snapshot(10).has_value());
  server.Wait();
  EXPECT_EQ(server.Requests().size(), 2U);
}

TEST(KalshiAdapterTest, TradingDayRegressionIsRejectedAndInvalidatesSnapshot) {
  LocalHttpServer server({
      R"({"market_positions":[],"cursor":""})",
      R"({"balance":10000,"portfolio_value":10000})",
  });
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());
  ASSERT_TRUE((*adapter)->RefreshAccountState(Context()));

  EXPECT_FALSE((*adapter)->RefreshAccountState(Context(19'999)));
  EXPECT_FALSE((*adapter)->CurrentCursor().has_value());
  EXPECT_FALSE((*adapter)->Snapshot(10).has_value());
  server.Wait();
  EXPECT_EQ(server.Requests().size(), 2U);
}

TEST(KalshiAdapterTest, DuplicateBoundMarketPositionFailsRefresh) {
  LocalHttpServer server({
      R"({"market_positions":[{"ticker":"TEST-MARKET","market_exposure_dollars":"90.000000"},{"ticker":"TEST-MARKET","market_exposure_dollars":"10.000000"}],"cursor":""})",
  });
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  EXPECT_FALSE((*adapter)->RefreshAccountState(Context()));
  EXPECT_FALSE((*adapter)->CurrentCursor().has_value());
  EXPECT_FALSE((*adapter)->Snapshot(10).has_value());
  server.Wait();
  EXPECT_EQ(server.Requests().size(), 1U);
}

TEST(KalshiAdapterTest, QueryRejectsOrderFromDifferentSubaccountAndDoesNotSendUndocumentedFilter) {
  const auto body = "{\"order\":{\"order_id\":\"venue-order-1\",\"client_order_id\":\"" + ClientOrderUuid(101) +
                    "\",\"ticker\":\"TEST-MARKET\",\"status\":\"resting\",\"book_side\":\"bid\",\"subaccount_number\":1,"
                    "\"yes_price_dollars\":\"0.2500\",\"initial_count_fp\":\"1.00\",\"fill_count_fp\":\"0.00\"}}";
  LocalHttpServer server({body});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Query(execution::VenueOrderLookupKey{
      .identity = risk::OrderIdentity{.strategy_id = 1,
                                      .client_order_id = 101,
                                      .market_id = 10,
                                      .side = risk::Side::kBuy,
                                      .price = risk::Price::FromCents(25),
                                      .quantity = risk::Quantity::FromWhole(1)},
      .venue_order_id = std::string{"venue-order-1"},
  });
  server.Wait();

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), execution::VenueOrderQueryError::kInvalidResponse);
  ASSERT_EQ(server.Requests().size(), 1U);
  EXPECT_NE(server.Requests()[0].find("GET /trade-api/v2/portfolio/orders/venue-order-1 HTTP"), std::string::npos);
  EXPECT_EQ(server.Requests()[0].find("subaccount="), std::string::npos);
}

TEST(KalshiAdapterTest, QueryRejectsWrongClientOrTicker) {
  const std::vector<std::string> bodies{
      "{\"order\":{\"order_id\":\"venue-order-1\",\"client_order_id\":\"wrong\",\"ticker\":\"TEST-MARKET\","
      "\"status\":\"resting\",\"book_side\":\"bid\",\"subaccount_number\":0,\"yes_price_dollars\":\"0.2500\","
      "\"initial_count_fp\":\"1.00\",\"fill_count_fp\":\"0.00\"}}",
      "{\"order\":{\"order_id\":\"venue-order-1\",\"client_order_id\":\"" + ClientOrderUuid(101) +
          "\",\"ticker\":\"OTHER-MARKET\",\"status\":\"resting\",\"book_side\":\"bid\",\"subaccount_number\":0,"
          "\"yes_price_dollars\":\"0.2500\",\"initial_count_fp\":\"1.00\",\"fill_count_fp\":\"0.00\"}}",
  };
  for (const auto& body : bodies) {
    LocalHttpServer server({body});
    if (!server.Available()) {
      GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
    }
    auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
    ASSERT_TRUE(adapter.has_value());
    const auto result = (*adapter)->Query(execution::VenueOrderLookupKey{
        .identity = risk::OrderIdentity{.strategy_id = 1,
                                        .client_order_id = 101,
                                        .market_id = 10,
                                        .side = risk::Side::kBuy,
                                        .price = risk::Price::FromCents(25),
                                        .quantity = risk::Quantity::FromWhole(1)},
        .venue_order_id = std::string{"venue-order-1"},
    });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), execution::VenueOrderQueryError::kInvalidResponse);
  }
}

TEST(KalshiAdapterTest, QueryRejectsWrongPrice) {
  LocalHttpServer server({OrderResponse("resting", "0.2600")});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Query(execution::VenueOrderLookupKey{
      .identity = Identity(),
      .venue_order_id = std::string{"venue-order-1"},
  });

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), execution::VenueOrderQueryError::kInvalidResponse);
}

TEST(KalshiAdapterTest, QueryRejectsWrongInitialQuantity) {
  LocalHttpServer server({OrderResponse("resting", "0.2500", "2.00")});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Query(execution::VenueOrderLookupKey{
      .identity = Identity(),
      .venue_order_id = std::string{"venue-order-1"},
  });

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), execution::VenueOrderQueryError::kInvalidResponse);
}

TEST(KalshiAdapterTest, CancelledOrderWithZeroFillsCanReconcile) {
  LocalHttpServer server({OrderResponse("canceled")});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Query(execution::VenueOrderLookupKey{
      .identity = Identity(),
      .venue_order_id = std::string{"venue-order-1"},
  });

  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ((*result)->state, execution::VenueOrderState::kCancelled);
}

TEST(KalshiAdapterTest, RejectedOrderWithFillIsInvalid) {
  LocalHttpServer server({OrderResponse("rejected", "0.2500", "1.00", "0.10")});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  const auto result = (*adapter)->Query(execution::VenueOrderLookupKey{
      .identity = Identity(),
      .venue_order_id = std::string{"venue-order-1"},
  });

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), execution::VenueOrderQueryError::kInvalidResponse);
}

TEST(KalshiAdapterTest, CancelledOrderWithPartialFillFailsStartupClosedAndKeepsReservation) {
  LocalHttpServer server({OrderResponse("canceled", "0.2500", "1.00", "0.30")});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());
  MemoryOrderStateStore store(UnknownEntry());
  risk::OrderRegistry registry;
  execution::KalshiStartupReconciler reconciler(**adapter, Context());

  const auto recovery = execution::StartupRecoverySession::Recover(
      registry, store, reconciler, **adapter, execution::AccountStateSafetyPolicy{.max_age = std::chrono::seconds(10)});

  ASSERT_FALSE(recovery.has_value());
  EXPECT_EQ(recovery.error(), execution::StartupRecoveryError::kReconciliationFailed);
  EXPECT_EQ(registry.State(101), risk::OrderState::kUnknown);
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{.micros = 250'000});
  server.Wait();
  ASSERT_EQ(server.Requests().size(), 1U);
}

TEST(KalshiAdapterTest, StartupRefreshesAccountStateAfterPersistedVenueReconciliation) {
  LocalHttpServer server({
      OrderResponse("canceled"),
      R"({"market_positions":[],"event_positions":[],"cursor":""})",
      R"({"balance":10000,"portfolio_value":10000})",
  });
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());
  MemoryOrderStateStore store(UnknownEntry());
  risk::OrderRegistry registry;
  execution::KalshiStartupReconciler reconciler(**adapter, Context());

  const auto recovery = execution::StartupRecoverySession::Recover(
      registry, store, reconciler, **adapter, execution::AccountStateSafetyPolicy{.max_age = std::chrono::seconds(10)});

  ASSERT_TRUE(recovery.has_value());
  EXPECT_EQ(registry.State(101), risk::OrderState::kCancelled);
  EXPECT_EQ(registry.TotalReservedExposure(), risk::Money{});
  EXPECT_TRUE((*adapter)->CurrentCursor().has_value());
  server.Wait();
  ASSERT_EQ(server.Requests().size(), 3U);
  EXPECT_NE(server.Requests()[0].find("GET /trade-api/v2/portfolio/orders/venue-order-1 HTTP"), std::string::npos);
  EXPECT_NE(server.Requests()[1].find("GET /trade-api/v2/portfolio/positions?"), std::string::npos);
  EXPECT_NE(server.Requests()[2].find("GET /trade-api/v2/portfolio/balance?"), std::string::npos);
}

TEST(KalshiAdapterTest, OverflowResponseFailsRefreshAndClearsPreviousSnapshot) {
  LocalHttpServer server({
      R"({"market_positions":[{"ticker":"TEST-MARKET","market_exposure_dollars":"12.500000"}],"cursor":""})",
      R"({"balance":8000,"portfolio_value":11000})",
      R"({"market_positions":[{"ticker":"TEST-MARKET","market_exposure_dollars":"9223372036854.999999"}],"cursor":""})",
  });
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());
  ASSERT_TRUE((*adapter)->RefreshAccountState(Context()));
  ASSERT_TRUE((*adapter)->CurrentCursor().has_value());
  ASSERT_TRUE((*adapter)->Snapshot(10).has_value());

  EXPECT_FALSE((*adapter)->RefreshAccountState(Context()));
  server.Wait();

  EXPECT_FALSE((*adapter)->CurrentCursor().has_value());
  EXPECT_FALSE((*adapter)->Snapshot(10).has_value());
}

TEST(KalshiAdapterTest, PaginationCursorFailsRefreshAndLeavesNoSnapshot) {
  LocalHttpServer server({R"({"market_positions":[],"cursor":"next-page"})"});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  EXPECT_FALSE((*adapter)->RefreshAccountState(Context()));
  server.Wait();
  EXPECT_FALSE((*adapter)->CurrentCursor().has_value());
  EXPECT_FALSE((*adapter)->Snapshot(10).has_value());
}

TEST(KalshiAdapterTest, MissingCursorFailsRefreshAndLeavesNoSnapshot) {
  LocalHttpServer server({R"({"market_positions":[]})"});
  if (!server.Available()) {
    GTEST_SKIP() << "loopback sockets unavailable: " << std::strerror(server.UnavailableErrno());
  }
  auto adapter = execution::KalshiAdapter::Create(Config(server.BaseUrl()));
  ASSERT_TRUE(adapter.has_value());

  EXPECT_FALSE((*adapter)->RefreshAccountState(Context()));
  server.Wait();
  EXPECT_FALSE((*adapter)->CurrentCursor().has_value());
  EXPECT_FALSE((*adapter)->Snapshot(10).has_value());
}

TEST(KalshiAdapterTest, EnforcesBotGuardV0SubaccountRangeThrough32) {
  auto maximum = Config("http://127.0.0.1:1/trade-api/v2");
  maximum.subaccount = 32;
  EXPECT_TRUE(execution::KalshiAdapter::Create(std::move(maximum)).has_value());

  auto outside_range = Config("http://127.0.0.1:1/trade-api/v2");
  outside_range.subaccount = 33;
  const auto rejected = execution::KalshiAdapter::Create(std::move(outside_range));
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error(), execution::KalshiAdapterCreateError::kInvalidConfig);
}

}  // namespace
