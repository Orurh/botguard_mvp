#include "uds_agent_server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <span>
#include <system_error>
#include <utility>

namespace botguard::agent {
namespace {

using Deadline = std::chrono::steady_clock::time_point;

enum class PeerIoResult : std::uint8_t {
  kReady,
  kTimedOut,
  kError,
};

[[nodiscard]] Deadline MakeDeadline(std::chrono::milliseconds timeout) noexcept {
  const auto now = std::chrono::steady_clock::now();
  const auto available = Deadline::max() - now;
  return timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(available) ? Deadline::max() : now + timeout;
}

[[nodiscard]] PeerIoResult WaitFor(int client, short events, Deadline deadline) noexcept {
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return PeerIoResult::kTimedOut;
    }
    const auto remaining = deadline - now;
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (std::chrono::duration_cast<Deadline::duration>(milliseconds) < remaining) {
      ++milliseconds;
    }
    const auto timeout = milliseconds.count() > INT_MAX ? INT_MAX : static_cast<int>(milliseconds.count());
    pollfd descriptor{.fd = client, .events = events, .revents = 0};
    const auto result = ::poll(&descriptor, 1, timeout);
    if (result == 0) {
      return PeerIoResult::kTimedOut;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return PeerIoResult::kError;
    }
    if ((descriptor.revents & events) != 0) {
      return PeerIoResult::kReady;
    }
    return PeerIoResult::kError;
  }
}

[[nodiscard]] PeerIoResult ReceiveFrame(int client, std::span<std::byte> frame, Deadline deadline, ssize_t& received) noexcept {
  while (true) {
    received = ::recv(client, frame.data(), frame.size(), MSG_TRUNC);
    if (received >= 0) {
      return PeerIoResult::kReady;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return PeerIoResult::kError;
    }
    const auto ready = WaitFor(client, POLLIN, deadline);
    if (ready != PeerIoResult::kReady) {
      return ready;
    }
  }
}

[[nodiscard]] PeerIoResult SendFrame(int client, std::span<const std::byte> frame, Deadline deadline) noexcept {
  while (true) {
    const auto sent = ::send(client, frame.data(), frame.size(), MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(frame.size())) {
      return PeerIoResult::kReady;
    }
    if (sent >= 0) {
      return PeerIoResult::kError;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return PeerIoResult::kError;
    }
    const auto ready = WaitFor(client, POLLOUT, deadline);
    if (ready != PeerIoResult::kReady) {
      return ready;
    }
  }
}

}  // namespace

std::expected<UdsAgentServer, UdsServerError> UdsAgentServer::Create(const std::filesystem::path& socket_path, AgentRequestHandler& handler,
                                                                     bool reclaim_stale_socket, UdsServerOptions options) noexcept {
  if (options.peer_io_timeout <= std::chrono::milliseconds::zero()) {
    return std::unexpected(UdsServerError::kInvalidTimeout);
  }
  const auto path = socket_path.string();
  sockaddr_un address{};
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    return std::unexpected(UdsServerError::kInvalidPath);
  }
  std::error_code error;
  if (std::filesystem::exists(socket_path, error)) {
    struct stat state{};
    if (!reclaim_stale_socket || ::lstat(path.c_str(), &state) != 0 || !S_ISSOCK(state.st_mode) || state.st_uid != ::geteuid()) {
      return std::unexpected(UdsServerError::kPathAlreadyExists);
    }
    const auto probe = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (probe < 0) {
      return std::unexpected(UdsServerError::kSocketFailed);
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const auto connected = ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    const auto connect_error = errno;
    ::close(probe);
    if (connected || connect_error != ECONNREFUSED || !std::filesystem::remove(socket_path, error) || error) {
      return std::unexpected(UdsServerError::kPathAlreadyExists);
    }
  }
  if (error) {
    return std::unexpected(UdsServerError::kInvalidPath);
  }

  const auto socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (socket < 0) {
    return std::unexpected(UdsServerError::kSocketFailed);
  }
  address = {};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socket);
    return std::unexpected(UdsServerError::kBindFailed);
  }
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    ::close(socket);
    static_cast<void>(std::filesystem::remove(socket_path, error));
    return std::unexpected(UdsServerError::kPermissionFailed);
  }
  if (::listen(socket, 16) != 0) {
    ::close(socket);
    static_cast<void>(std::filesystem::remove(socket_path, error));
    return std::unexpected(UdsServerError::kListenFailed);
  }
  return UdsAgentServer(socket, socket_path, handler, options);
}

UdsAgentServer::~UdsAgentServer() {
  Close();
}

UdsAgentServer::UdsAgentServer(UdsAgentServer&& other) noexcept
    : socket_(std::exchange(other.socket_, -1)),
      socket_path_(std::move(other.socket_path_)),
      handler_(other.handler_),
      options_(other.options_) {
  other.handler_ = nullptr;
}

ServeOneResult UdsAgentServer::ServeOne() noexcept {
  const auto client = ::accept4(socket_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (client < 0) {
    return ServeOneResult::kTransportError;
  }

  const auto receive_deadline = MakeDeadline(options_.peer_io_timeout);
  std::array<std::byte, kMaxRequestFrameSize> buffer{};
  ssize_t received{};
  const auto receive_result = ReceiveFrame(client, buffer, receive_deadline, received);
  if (receive_result != PeerIoResult::kReady) {
    ::close(client);
    return receive_result == PeerIoResult::kTimedOut ? ServeOneResult::kPeerTimedOut : ServeOneResult::kTransportError;
  }
  if (received < 0 || received > static_cast<ssize_t>(buffer.size())) {
    ::close(client);
    return received < 0 ? ServeOneResult::kTransportError : ServeOneResult::kInvalidRequest;
  }
  const auto request = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(received));
  const auto header = DecodeHeader(request);
  if (!header) {
    ::close(client);
    return ServeOneResult::kInvalidRequest;
  }

  auto result = ServeOneResult::kHandled;
  auto send_result = PeerIoResult::kError;
  switch (header->message_type) {
    case MessageType::kSubmitIntent: {
      SubmitResponse response;
      const auto intent = DecodeSubmitIntent(request);
      if (!intent) {
        response.status = ResponseStatus::kInvalidRequest;
        result = ServeOneResult::kInvalidRequest;
      } else {
        try {
          response = MakeSubmitResponse(handler_->HandleSubmit(*intent));
        } catch (...) {
          response.status = ResponseStatus::kInternalError;
        }
      }
      const auto encoded = EncodeSubmitResponse(response);
      send_result = SendFrame(client, encoded, MakeDeadline(options_.peer_io_timeout));
      break;
    }
    case MessageType::kGetStatus: {
      if (!DecodeEmptyRequest(request, MessageType::kGetStatus)) {
        result = ServeOneResult::kInvalidRequest;
        break;
      }
      try {
        const auto encoded = EncodeStatusResponse(handler_->GetStatus(), MessageType::kStatusResponse);
        send_result = SendFrame(client, encoded, MakeDeadline(options_.peer_io_timeout));
      } catch (...) {
        result = ServeOneResult::kTransportError;
      }
      break;
    }
    case MessageType::kKill: {
      if (!DecodeEmptyRequest(request, MessageType::kKill)) {
        result = ServeOneResult::kInvalidRequest;
        break;
      }
      try {
        const auto status = handler_->Kill();
        if (status) {
          const auto encoded = EncodeStatusResponse(*status, MessageType::kKillResponse);
          send_result = SendFrame(client, encoded, MakeDeadline(options_.peer_io_timeout));
        } else {
          result = ServeOneResult::kTransportError;
        }
      } catch (...) {
        result = ServeOneResult::kTransportError;
      }
      break;
    }
    case MessageType::kResume: {
      if (!DecodeEmptyRequest(request, MessageType::kResume)) {
        result = ServeOneResult::kInvalidRequest;
        break;
      }
      try {
        const auto encoded = EncodeResumeResponse(handler_->Resume());
        send_result = SendFrame(client, encoded, MakeDeadline(options_.peer_io_timeout));
      } catch (...) {
        result = ServeOneResult::kTransportError;
      }
      break;
    }
    case MessageType::kSubmitResponse:
    case MessageType::kStatusResponse:
    case MessageType::kKillResponse:
    case MessageType::kResumeResponse:
      result = ServeOneResult::kInvalidRequest;
      break;
  }
  ::close(client);
  if (result == ServeOneResult::kInvalidRequest && send_result != PeerIoResult::kReady) {
    return result;
  }
  if (send_result == PeerIoResult::kTimedOut) {
    return ServeOneResult::kPeerTimedOut;
  }
  return send_result == PeerIoResult::kReady ? result : ServeOneResult::kTransportError;
}

UdsAgentServer::UdsAgentServer(int socket, std::filesystem::path socket_path, AgentRequestHandler& handler,
                               UdsServerOptions options) noexcept
    : socket_(socket), socket_path_(std::move(socket_path)), handler_(&handler), options_(options) {}

void UdsAgentServer::Close() noexcept {
  if (socket_ >= 0) {
    ::close(socket_);
    socket_ = -1;
  }
  if (!socket_path_.empty()) {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(socket_path_, error));
    socket_path_.clear();
  }
}

}  // namespace botguard::agent
