#include "execution/runtime_lease.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace execution = botguard::execution;

class TemporaryLeasePath final {
 public:
  TemporaryLeasePath() {
    static std::atomic_uint64_t sequence{};
    path_ = std::filesystem::temp_directory_path() /
            ("botguard_runtime_lease_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(sequence.fetch_add(1)) + ".db");
  }

  ~TemporaryLeasePath() {
    std::error_code error;
    static_cast<void>(std::filesystem::remove(path_.string() + ".lock", error));
  }

  [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(RuntimeLeaseTest, LeaseCanBeReacquiredAfterRelease) {
  TemporaryLeasePath path;
  {
    const auto first = execution::RuntimeLease::Acquire(path.Path());
    ASSERT_TRUE(first.has_value());
  }
  const auto second = execution::RuntimeLease::Acquire(path.Path());
  EXPECT_TRUE(second.has_value());
}

#if defined(__unix__) || defined(__APPLE__)
TEST(RuntimeLeaseTest, SecondProcessFailsClosedWhileLeaseIsHeld) {
  TemporaryLeasePath path;
  int ready_pipe[2]{};
  int release_pipe[2]{};
  ASSERT_EQ(pipe(ready_pipe), 0);
  ASSERT_EQ(pipe(release_pipe), 0);

  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    static_cast<void>(close(ready_pipe[0]));
    static_cast<void>(close(release_pipe[1]));
    const auto lease = execution::RuntimeLease::Acquire(path.Path());
    const char ready = lease ? '1' : '0';
    if (write(ready_pipe[1], &ready, 1) != 1 || !lease) {
      _exit(10);
    }
    char release{};
    if (read(release_pipe[0], &release, 1) != 1) {
      _exit(11);
    }
    _exit(0);
  }

  static_cast<void>(close(ready_pipe[1]));
  static_cast<void>(close(release_pipe[0]));
  char ready{};
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  ASSERT_EQ(ready, '1');

  const auto competing = execution::RuntimeLease::Acquire(path.Path());
  ASSERT_FALSE(competing.has_value());
  EXPECT_EQ(competing.error(), execution::RuntimeLeaseError::kAlreadyHeld);

  const char release = '1';
  ASSERT_EQ(write(release_pipe[1], &release, 1), 1);
  static_cast<void>(close(ready_pipe[0]));
  static_cast<void>(close(release_pipe[1]));
  int status{};
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

}  // namespace
