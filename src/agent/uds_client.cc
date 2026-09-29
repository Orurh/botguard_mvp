#include "uds_client.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstring>

namespace botguard::agent {
namespace {

using Deadline = std::chrono::steady_clock::time_point;

[[nodiscard]] Deadline MakeDeadline(std::chrono::milliseconds timeout) noexcept {
  const auto now = std::chrono::steady_clock::now();
  const auto available = Deadline::max() - now;
  return timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(available) ? Deadline::max() : now + timeout;
}

[[nodiscard]] std::expected<void, UdsClientError> WaitFor(int socket, short events, Deadline deadline,
                                                          UdsClientError operation_error) noexcept {
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return std::unexpected(UdsClientError::kTimedOut);
    }
    const auto remaining = deadline - now;
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (std::chrono::duration_cast<Deadline::duration>(milliseconds) < remaining) {
      ++milliseconds;
    }
    const auto timeout = milliseconds.count() > INT_MAX ? INT_MAX : static_cast<int>(milliseconds.count());
    pollfd descriptor{.fd = socket, .events = events, .revents = 0};
    const auto result = ::poll(&descriptor, 1, timeout);
    if (result == 0) {
      return std::unexpected(UdsClientError::kTimedOut);
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return std::unexpected(operation_error);
    }
    if ((descriptor.revents & events) != 0) {
      return {};
    }
    return std::unexpected(operation_error);
  }
}

[[nodiscard]] std::expected<void, UdsClientError> Connect(int socket, const sockaddr_un& address, Deadline deadline) noexcept {
  while (true) {
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 || errno == EISCONN) {
      return {};
    }
    if (errno == EINTR) {
      continue;
    }
    const auto connect_error = errno;
    if (connect_error != EINPROGRESS && connect_error != EAGAIN && connect_error != EWOULDBLOCK) {
      return std::unexpected(UdsClientError::kConnectFailed);
    }
    const auto ready = WaitFor(socket, POLLOUT, deadline, UdsClientError::kConnectFailed);
    if (!ready) {
      return ready;
    }
    if (connect_error == EAGAIN || connect_error == EWOULDBLOCK) {
      continue;
    }
    int socket_error{};
    socklen_t size = sizeof(socket_error);
    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &socket_error, &size) != 0 || size != sizeof(socket_error) || socket_error != 0) {
      return std::unexpected(UdsClientError::kConnectFailed);
    }
    return {};
  }
}

template <std::size_t Size>
[[nodiscard]] std::expected<void, UdsClientError> Send(int socket, const std::array<std::byte, Size>& request, Deadline deadline) noexcept {
  while (true) {
    const auto sent = ::send(socket, request.data(), request.size(), MSG_NOSIGNAL);
    if (sent == static_cast<ssize_t>(request.size())) {
      return {};
    }
    if (sent >= 0) {
      return std::unexpected(UdsClientError::kSendFailed);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return std::unexpected(UdsClientError::kSendFailed);
    }
    const auto ready = WaitFor(socket, POLLOUT, deadline, UdsClientError::kSendFailed);
    if (!ready) {
      return ready;
    }
  }
}

template <std::size_t Size>
[[nodiscard]] std::expected<std::array<std::byte, Size>, UdsClientError> Receive(int socket, Deadline deadline) noexcept {
  std::array<std::byte, Size> response{};
  while (true) {
    const auto received = ::recv(socket, response.data(), response.size(), MSG_TRUNC);
    if (received == static_cast<ssize_t>(response.size())) {
      return response;
    }
    if (received >= 0) {
      return std::unexpected(UdsClientError::kReceiveFailed);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return std::unexpected(UdsClientError::kReceiveFailed);
    }
    const auto ready = WaitFor(socket, POLLIN, deadline, UdsClientError::kReceiveFailed);
    if (!ready) {
      return std::unexpected(ready.error());
    }
  }
}

template <std::size_t RequestSize, std::size_t ResponseSize>
[[nodiscard]] std::expected<std::array<std::byte, ResponseSize>, UdsClientError> Exchange(const std::filesystem::path& socket_path,
                                                                                          const std::array<std::byte, RequestSize>& request,
                                                                                          UdsClientOptions options) noexcept {
  if (options.timeout <= std::chrono::milliseconds::zero()) {
    return std::unexpected(UdsClientError::kInvalidTimeout);
  }
  const auto& path = socket_path.native();
  sockaddr_un address{};
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    return std::unexpected(UdsClientError::kInvalidPath);
  }
  const auto socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (socket < 0) {
    return std::unexpected(UdsClientError::kSocketFailed);
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const auto deadline = MakeDeadline(options.timeout);
  const auto connected = Connect(socket, address, deadline);
  if (!connected) {
    ::close(socket);
    return std::unexpected(connected.error());
  }
  const auto sent = Send(socket, request, deadline);
  if (!sent) {
    ::close(socket);
    return std::unexpected(sent.error());
  }
  auto response = Receive<ResponseSize>(socket, deadline);
  ::close(socket);
  return response;
}

}  // namespace

std::expected<SubmitResponse, UdsClientError> SubmitOverUds(const std::filesystem::path& socket_path, const risk::OrderIntent& intent,
                                                            UdsClientOptions options) noexcept {
  const auto response = Exchange<kSubmitIntentFrameSize, kSubmitResponseFrameSize>(socket_path, EncodeSubmitIntent(intent), options);
  if (!response) {
    return std::unexpected(response.error());
  }
  const auto decoded = DecodeSubmitResponse(*response);
  return decoded ? std::expected<SubmitResponse, UdsClientError>{*decoded} : std::unexpected(UdsClientError::kInvalidResponse);
}

std::expected<StatusSnapshot, UdsClientError> GetStatusOverUds(const std::filesystem::path& socket_path,
                                                               UdsClientOptions options) noexcept {
  const auto response = Exchange<kGetStatusFrameSize, kStatusResponseFrameSize>(socket_path, EncodeGetStatus(), options);
  if (!response) {
    return std::unexpected(response.error());
  }
  const auto decoded = DecodeStatusResponse(*response, MessageType::kStatusResponse);
  return decoded ? std::expected<StatusSnapshot, UdsClientError>{*decoded} : std::unexpected(UdsClientError::kInvalidResponse);
}

std::expected<StatusSnapshot, UdsClientError> KillOverUds(const std::filesystem::path& socket_path, UdsClientOptions options) noexcept {
  const auto response = Exchange<kKillFrameSize, kStatusResponseFrameSize>(socket_path, EncodeKill(), options);
  if (!response) {
    return std::unexpected(response.error());
  }
  const auto decoded = DecodeStatusResponse(*response, MessageType::kKillResponse);
  return decoded ? std::expected<StatusSnapshot, UdsClientError>{*decoded} : std::unexpected(UdsClientError::kInvalidResponse);
}

std::expected<ResumeResponse, UdsClientError> ResumeOverUds(const std::filesystem::path& socket_path, UdsClientOptions options) noexcept {
  const auto response = Exchange<kResumeFrameSize, kResumeResponseFrameSize>(socket_path, EncodeResume(), options);
  if (!response) {
    return std::unexpected(response.error());
  }
  const auto decoded = DecodeResumeResponse(*response);
  return decoded ? std::expected<ResumeResponse, UdsClientError>{*decoded} : std::unexpected(UdsClientError::kInvalidResponse);
}

}  // namespace botguard::agent
