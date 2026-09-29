#pragma once

#include "uds_protocol.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>

namespace botguard::agent {

enum class AgentControlError : std::uint8_t {
  kPersistenceFailed,
};

class AgentRequestHandler {
 public:
  virtual ~AgentRequestHandler() = default;
  [[nodiscard]] virtual execution::SubmissionResult HandleSubmit(const risk::OrderIntent& intent) = 0;
  [[nodiscard]] virtual StatusSnapshot GetStatus() const = 0;
  [[nodiscard]] virtual std::expected<StatusSnapshot, AgentControlError> Kill() = 0;
  [[nodiscard]] virtual ResumeResponse Resume() = 0;
};

enum class UdsServerError : std::uint8_t {
  kInvalidTimeout,
  kInvalidPath,
  kPathAlreadyExists,
  kSocketFailed,
  kBindFailed,
  kPermissionFailed,
  kListenFailed,
};

enum class ServeOneResult : std::uint8_t {
  kHandled,
  kInvalidRequest,
  kTransportError,
  kPeerTimedOut,
};

struct UdsServerOptions {
  std::chrono::milliseconds peer_io_timeout{std::chrono::seconds{2}};
};

class UdsAgentServer final {
 public:
  [[nodiscard]] static std::expected<UdsAgentServer, UdsServerError> Create(const std::filesystem::path& socket_path,
                                                                            AgentRequestHandler& handler, bool reclaim_stale_socket = false,
                                                                            UdsServerOptions options = {}) noexcept;

  ~UdsAgentServer();
  UdsAgentServer(const UdsAgentServer&) = delete;
  UdsAgentServer& operator=(const UdsAgentServer&) = delete;
  UdsAgentServer(UdsAgentServer&& other) noexcept;
  UdsAgentServer& operator=(UdsAgentServer&&) = delete;

  [[nodiscard]] ServeOneResult ServeOne() noexcept;

 private:
  UdsAgentServer(int socket, std::filesystem::path socket_path, AgentRequestHandler& handler, UdsServerOptions options) noexcept;
  void Close() noexcept;

  int socket_{-1};
  std::filesystem::path socket_path_;
  AgentRequestHandler* handler_{};
  UdsServerOptions options_{};
};

}  // namespace botguard::agent
