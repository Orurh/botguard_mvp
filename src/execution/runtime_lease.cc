#include "runtime_lease.h"

#include <cerrno>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace botguard::execution {

std::expected<RuntimeLease, RuntimeLeaseError> RuntimeLease::Acquire(const std::filesystem::path& order_database_path) noexcept {
#if defined(__linux__) || defined(__APPLE__)
  try {
    auto lock_path = order_database_path;
    lock_path += ".lock";
    const auto path = lock_path.string();
    if (path.empty()) {
      return std::unexpected(RuntimeLeaseError::kOpenFailed);
    }

    int flags = O_CREAT | O_RDWR | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const auto descriptor = open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
      return std::unexpected(RuntimeLeaseError::kOpenFailed);
    }

    struct stat metadata{};
    if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1) {
      static_cast<void>(close(descriptor));
      return std::unexpected(RuntimeLeaseError::kUnsafeLockFile);
    }

    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
      const auto error = errno;
      static_cast<void>(close(descriptor));
      return std::unexpected(error == EWOULDBLOCK || error == EAGAIN ? RuntimeLeaseError::kAlreadyHeld : RuntimeLeaseError::kOpenFailed);
    }
    return RuntimeLease(descriptor);
  } catch (...) {
    return std::unexpected(RuntimeLeaseError::kOpenFailed);
  }
#else
  static_cast<void>(order_database_path);
  return std::unexpected(RuntimeLeaseError::kUnsupportedPlatform);
#endif
}

RuntimeLease::~RuntimeLease() {
  Close();
}

RuntimeLease::RuntimeLease(RuntimeLease&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

RuntimeLease& RuntimeLease::operator=(RuntimeLease&& other) noexcept {
  if (this != &other) {
    Close();
    descriptor_ = std::exchange(other.descriptor_, -1);
  }
  return *this;
}

void RuntimeLease::Close() noexcept {
#if defined(__linux__) || defined(__APPLE__)
  if (descriptor_ >= 0) {
    static_cast<void>(flock(descriptor_, LOCK_UN));
    static_cast<void>(close(descriptor_));
    descriptor_ = -1;
  }
#endif
}

}  // namespace botguard::execution
