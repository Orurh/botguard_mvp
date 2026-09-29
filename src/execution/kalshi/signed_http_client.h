#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace botguard::execution::kalshi_internal {

struct HttpResponse {
  bool transport_ok{};
  long status{};
  std::string body;
};

enum class SignedHttpClientCreateError : std::uint8_t {
  kInvalidConfig,
  kInvalidPrivateKey,
  kTransportInitializationFailed,
};

class SignedHttpClient {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<SignedHttpClient>, SignedHttpClientCreateError> Create(
      std::string base_url, std::string api_key_id, std::string private_key_pem, std::chrono::milliseconds request_timeout) noexcept;

  ~SignedHttpClient();

  SignedHttpClient(const SignedHttpClient&) = delete;
  SignedHttpClient& operator=(const SignedHttpClient&) = delete;

  [[nodiscard]] HttpResponse Request(std::string_view method, std::string_view endpoint, std::string_view query = {},
                                     std::string_view body = {}) const;

 private:
  class Impl;
  explicit SignedHttpClient(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace botguard::execution::kalshi_internal
