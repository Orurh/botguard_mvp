#include "signed_http_client.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace botguard::execution::kalshi_internal {
namespace {

constexpr std::size_t kMaxResponseBytes = 1U << 20U;

[[nodiscard]] std::size_t AppendBody(char* data, std::size_t size, std::size_t count, void* context) noexcept {
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    return 0;
  }
  const auto bytes = size * count;
  auto& output = *static_cast<std::string*>(context);
  if (bytes > kMaxResponseBytes - std::min(output.size(), kMaxResponseBytes)) {
    return 0;
  }
  output.append(data, bytes);
  return bytes;
}

}  // namespace

class SignedHttpClient::Impl {
 public:
  Impl(std::string base_url, std::string api_key_id, std::chrono::milliseconds request_timeout, EVP_PKEY* private_key, std::string api_path)
      : base_url_(std::move(base_url)),
        api_key_id_(std::move(api_key_id)),
        request_timeout_(request_timeout),
        private_key_(private_key),
        api_path_(std::move(api_path)) {}

  ~Impl() { EVP_PKEY_free(private_key_); }

  [[nodiscard]] std::optional<std::string> Signature(std::string_view timestamp, std::string_view method, std::string_view endpoint) const {
    const auto message = std::string(timestamp) + std::string(method) + api_path_ + std::string(endpoint);
    EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
    if (raw_context == nullptr) {
      return std::nullopt;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw_context, EVP_MD_CTX_free);
    EVP_PKEY_CTX* key_context = nullptr;
    if (EVP_DigestSignInit(context.get(), &key_context, EVP_sha256(), nullptr, private_key_) != 1 || key_context == nullptr ||
        EVP_PKEY_CTX_set_rsa_padding(key_context, RSA_PKCS1_PSS_PADDING) <= 0 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(key_context, RSA_PSS_SALTLEN_DIGEST) <= 0 ||
        EVP_DigestSignUpdate(context.get(), message.data(), message.size()) != 1) {
      return std::nullopt;
    }
    std::size_t signature_size{};
    if (EVP_DigestSignFinal(context.get(), nullptr, &signature_size) != 1) {
      return std::nullopt;
    }
    std::vector<unsigned char> signature(signature_size);
    if (EVP_DigestSignFinal(context.get(), signature.data(), &signature_size) != 1) {
      return std::nullopt;
    }
    signature.resize(signature_size);
    std::string encoded(4 * ((signature.size() + 2) / 3), '\0');
    const auto encoded_size =
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), signature.data(), static_cast<int>(signature.size()));
    if (encoded_size < 0) {
      return std::nullopt;
    }
    encoded.resize(static_cast<std::size_t>(encoded_size));
    return encoded;
  }

  std::string base_url_;
  std::string api_key_id_;
  std::chrono::milliseconds request_timeout_{};
  EVP_PKEY* private_key_{};
  std::string api_path_;
};

SignedHttpClient::SignedHttpClient(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
SignedHttpClient::~SignedHttpClient() = default;

std::expected<std::unique_ptr<SignedHttpClient>, SignedHttpClientCreateError> SignedHttpClient::Create(
    std::string base_url, std::string api_key_id, std::string private_key_pem, std::chrono::milliseconds request_timeout) noexcept {
  try {
    const auto scheme = base_url.find("://");
    const auto path_start = scheme == std::string::npos ? std::string::npos : base_url.find('/', scheme + 3);
    if (base_url.empty() || base_url.back() == '/' || api_key_id.empty() || private_key_pem.empty() ||
        private_key_pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        request_timeout <= std::chrono::milliseconds::zero() || request_timeout > std::chrono::minutes(5) ||
        path_start == std::string::npos) {
      return std::unexpected(SignedHttpClientCreateError::kInvalidConfig);
    }
    BIO* raw_bio = BIO_new_mem_buf(private_key_pem.data(), static_cast<int>(private_key_pem.size()));
    if (raw_bio == nullptr) {
      return std::unexpected(SignedHttpClientCreateError::kInvalidPrivateKey);
    }
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(raw_bio, BIO_free);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA) {
      return std::unexpected(SignedHttpClientCreateError::kInvalidPrivateKey);
    }
    static const auto curl_status = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_status != CURLE_OK) {
      return std::unexpected(SignedHttpClientCreateError::kTransportInitializationFailed);
    }
    auto api_path = base_url.substr(path_start);
    auto impl = std::make_unique<Impl>(std::move(base_url), std::move(api_key_id), request_timeout, key.release(), std::move(api_path));
    return std::unique_ptr<SignedHttpClient>(new SignedHttpClient(std::move(impl)));
  } catch (...) {
    return std::unexpected(SignedHttpClientCreateError::kInvalidConfig);
  }
}

HttpResponse SignedHttpClient::Request(std::string_view method, std::string_view endpoint, std::string_view query,
                                       std::string_view body) const {
  HttpResponse response;
  const auto timestamp =
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  const auto signature = impl_->Signature(timestamp, method, endpoint);
  if (!signature) {
    return response;
  }
  CURL* raw_curl = curl_easy_init();
  if (raw_curl == nullptr) {
    return response;
  }
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw_curl, curl_easy_cleanup);
  const auto url = impl_->base_url_ + std::string(endpoint) + std::string(query);
  const auto method_text = std::string(method);
  curl_slist* raw_headers = nullptr;
  const auto append = [&raw_headers](const std::string& header) {
    auto* updated = curl_slist_append(raw_headers, header.c_str());
    if (updated == nullptr) {
      return false;
    }
    raw_headers = updated;
    return true;
  };
  if (!append("KALSHI-ACCESS-KEY: " + impl_->api_key_id_) || !append("KALSHI-ACCESS-TIMESTAMP: " + timestamp) ||
      !append("KALSHI-ACCESS-SIGNATURE: " + *signature) || !append("Content-Type: application/json")) {
    curl_slist_free_all(raw_headers);
    return response;
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
  if (curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method_text.c_str()) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(impl_->request_timeout_.count())) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, AppendBody) != CURLE_OK ||
      curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body) != CURLE_OK) {
    return response;
  }
  if (!body.empty()) {
    if (body.size() > static_cast<std::size_t>(std::numeric_limits<long>::max()) ||
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data()) != CURLE_OK ||
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size())) != CURLE_OK) {
      return response;
    }
  }
  response.transport_ok = curl_easy_perform(curl.get()) == CURLE_OK;
  if (response.transport_ok) {
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
  }
  return response;
}

}  // namespace botguard::execution::kalshi_internal
