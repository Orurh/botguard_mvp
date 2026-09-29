#pragma once

#include "uds_protocol.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>

namespace botguard::agent {

enum class UdsClientError : std::uint8_t {
  kInvalidTimeout,
  kInvalidPath,
  kSocketFailed,
  kConnectFailed,
  kSendFailed,
  kReceiveFailed,
  kInvalidResponse,
  kTimedOut,
};

struct UdsClientOptions {
  // One end-to-end deadline shared by connect, send, and receive.
  std::chrono::milliseconds timeout{std::chrono::seconds{5}};
};

[[nodiscard]] std::expected<SubmitResponse, UdsClientError> SubmitOverUds(const std::filesystem::path& socket_path,
                                                                          const risk::OrderIntent& intent,
                                                                          UdsClientOptions options = {}) noexcept;
[[nodiscard]] std::expected<StatusSnapshot, UdsClientError> GetStatusOverUds(const std::filesystem::path& socket_path,
                                                                             UdsClientOptions options = {}) noexcept;
[[nodiscard]] std::expected<StatusSnapshot, UdsClientError> KillOverUds(const std::filesystem::path& socket_path,
                                                                        UdsClientOptions options = {}) noexcept;
[[nodiscard]] std::expected<ResumeResponse, UdsClientError> ResumeOverUds(const std::filesystem::path& socket_path,
                                                                          UdsClientOptions options = {}) noexcept;

}  // namespace botguard::agent
