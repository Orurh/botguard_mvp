#include "agent/uds_agent_server.h"
#include "agent/uds_client.h"
#include "demo/persistent_fault_injecting_venue.h"
#include "demo/simulator_runtime.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

namespace agent = botguard::agent;
namespace demo = botguard::demo;
namespace execution = botguard::execution;
namespace risk = botguard::risk;

class TemporaryAgentFiles final {
 public:
  TemporaryAgentFiles() {
    static std::atomic_uint64_t sequence{};
    root_ = std::filesystem::temp_directory_path() /
            ("botguard_uds_killer_" + std::to_string(::getpid()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(sequence.fetch_add(1)));
    std::error_code error;
    created_ = std::filesystem::create_directory(root_, error) && !error;
  }

  ~TemporaryAgentFiles() {
    if (created_) {
      std::error_code error;
      static_cast<void>(std::filesystem::remove_all(root_, error));
    }
  }

  [[nodiscard]] bool Created() const noexcept { return created_; }
  [[nodiscard]] std::filesystem::path Socket() const { return root_ / "agent.sock"; }
  [[nodiscard]] std::filesystem::path Orders() const { return root_ / "orders.db"; }
  [[nodiscard]] std::filesystem::path Runtime() const { return root_ / "orders.db.runtime"; }
  [[nodiscard]] std::filesystem::path Venue() const { return root_ / "venue.db"; }

 private:
  std::filesystem::path root_;
  bool created_{};
};

class AgentProcess final {
 public:
  explicit AgentProcess(pid_t process) noexcept : process_(process) {}
  ~AgentProcess() { Stop(); }
  AgentProcess(const AgentProcess&) = delete;
  AgentProcess& operator=(const AgentProcess&) = delete;

  void Stop() noexcept {
    if (process_ <= 0) {
      return;
    }
    static_cast<void>(::kill(process_, SIGKILL));
    static_cast<void>(::waitpid(process_, nullptr, 0));
    process_ = -1;
  }

 private:
  pid_t process_{};
};

[[nodiscard]] bool UdsSocketsAvailable(const std::filesystem::path& path, int& unavailable_errno) {
  const auto socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (socket < 0) {
    unavailable_errno = errno;
    return false;
  }
  sockaddr_un address{};
  const auto value = path.string();
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, value.c_str(), value.size() + 1);
  const auto available = ::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
  unavailable_errno = available ? 0 : errno;
  ::close(socket);
  std::error_code error;
  static_cast<void>(std::filesystem::remove(path, error));
  return available;
}

[[nodiscard]] risk::OrderIntent Intent(risk::ClientOrderId client_order_id = 1001) noexcept {
  return risk::OrderIntent{
      .strategy_id = 1,
      .client_order_id = client_order_id,
      .market_id = 10,
      .side = risk::Side::kBuy,
      .price = risk::Price::FromCents(25),
      .quantity = risk::Quantity::FromWhole(4),
      .market_data_received_at = {},
  };
}

[[nodiscard]] bool ExecuteSql(const std::filesystem::path& path, const char* sql) {
  sqlite3* database{};
  const auto value = path.string();
  if (sqlite3_open_v2(value.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
    if (database != nullptr) {
      static_cast<void>(sqlite3_close(database));
    }
    return false;
  }
  const auto executed = sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  return sqlite3_close(database) == SQLITE_OK && executed;
}

[[nodiscard]] pid_t StartAgent(const TemporaryAgentFiles& files, bool lose_response,
                               std::chrono::milliseconds peer_io_timeout = std::chrono::seconds{2}) {
  int ready[2]{};
  if (::pipe(ready) != 0) {
    return -1;
  }
  const auto child = ::fork();
  if (child < 0) {
    ::close(ready[0]);
    ::close(ready[1]);
    return -1;
  }
  if (child == 0) {
    ::close(ready[0]);
    auto runtime = demo::SimulatorRuntime::Create({
        .order_database_path = files.Orders(),
        .venue_database_path = files.Venue(),
        .lose_submit_response = lose_response,
        .safety_inputs = nullptr,
        .crash_after_rollover_baseline_persisted = false,
    });
    auto server =
        runtime
            ? agent::UdsAgentServer::Create(files.Socket(), **runtime, true, agent::UdsServerOptions{.peer_io_timeout = peer_io_timeout})
            : std::expected<agent::UdsAgentServer, agent::UdsServerError>{std::unexpected(agent::UdsServerError::kSocketFailed)};
    const char result = runtime && server ? 'R' : 'E';
    const auto written = ::write(ready[1], &result, 1);
    ::close(ready[1]);
    if (written != 1 || !runtime || !server) {
      ::_exit(1);
    }
    while (true) {
      static_cast<void>(server->ServeOne());
    }
  }
  ::close(ready[1]);
  char result = 'E';
  const auto read_count = ::read(ready[0], &result, 1);
  ::close(ready[0]);
  if (read_count != 1 || result != 'R') {
    static_cast<void>(::kill(child, SIGKILL));
    static_cast<void>(::waitpid(child, nullptr, 0));
    return -1;
  }
  return child;
}

TEST(UdsKillerIntegrationTest, LiveGateDegradationIsReportedAndResumeRunsRecovery) {
  TemporaryAgentFiles files;
  ASSERT_TRUE(files.Created());
  int unavailable_errno = 0;
  if (!UdsSocketsAvailable(files.Socket(), unavailable_errno)) {
    GTEST_SKIP() << "Unix SOCK_SEQPACKET unavailable: " << std::strerror(unavailable_errno);
  }

  const auto agent_process = StartAgent(files, false);
  ASSERT_GT(agent_process, 0);
  AgentProcess process(agent_process);
  const auto initial = agent::GetStatusOverUds(files.Socket());
  ASSERT_TRUE(initial.has_value());
  ASSERT_EQ(initial->agent_state, agent::AgentState::kReady);

  ASSERT_TRUE(ExecuteSql(files.Orders(), R"sql(
    CREATE TRIGGER fail_final_order_state
    BEFORE UPDATE ON order_state
    WHEN NEW.state != 0
    BEGIN
      SELECT RAISE(ABORT, 'injected final persistence failure');
    END;
  )sql"));

  const auto submit = agent::SubmitOverUds(files.Socket(), Intent());
  ASSERT_TRUE(submit.has_value());
  EXPECT_EQ(submit->status, agent::ResponseStatus::kPostSubmitPersistenceFailed);

  const auto degraded = agent::GetStatusOverUds(files.Socket());
  ASSERT_TRUE(degraded.has_value());
  EXPECT_EQ(degraded->agent_state, agent::AgentState::kRecoveryRequired);
  EXPECT_EQ(degraded->gate_state, agent::WireGateState::kReconciliationRequired);

  const auto blocked = agent::SubmitOverUds(files.Socket(), Intent(1002));
  ASSERT_TRUE(blocked.has_value());
  EXPECT_EQ(blocked->status, agent::ResponseStatus::kRiskRejected);
  EXPECT_TRUE(agent::HasRejectReason(*blocked, agent::WireRejectReason::kReconciliationRequired));

  const auto resume = agent::ResumeOverUds(files.Socket());
  ASSERT_TRUE(resume.has_value());
  EXPECT_NE(resume->result, agent::WireResumeResult::kAlreadyReady);
  EXPECT_EQ(resume->result, agent::WireResumeResult::kRecoveryFailed);

  auto venue = demo::PersistentFaultInjectingVenue::Open(files.Venue(), false);
  ASSERT_TRUE(venue.has_value());
  EXPECT_EQ(venue->SubmitCount(), 1U);
}

TEST(UdsAgentServerTest, SilentAcceptedPeerCannotHoldExecutionThreadIndefinitely) {
  TemporaryAgentFiles files;
  ASSERT_TRUE(files.Created());
  int unavailable_errno = 0;
  if (!UdsSocketsAvailable(files.Socket(), unavailable_errno)) {
    GTEST_SKIP() << "Unix SOCK_SEQPACKET unavailable: " << std::strerror(unavailable_errno);
  }

  const auto agent_process = StartAgent(files, false, std::chrono::milliseconds{50});
  ASSERT_GT(agent_process, 0);
  AgentProcess process(agent_process);

  const auto silent = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(silent, 0);
  sockaddr_un address{};
  const auto path = files.Socket().string();
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  ASSERT_EQ(::connect(silent, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

  const auto started = std::chrono::steady_clock::now();
  const auto status = agent::GetStatusOverUds(files.Socket());
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ::close(silent);

  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->agent_state, agent::AgentState::kReady);
  EXPECT_GE(elapsed, std::chrono::milliseconds{40});
  EXPECT_LT(elapsed, std::chrono::seconds{1});
}

TEST(UdsKillerIntegrationTest, DurableKillAndControlledResumeNeverCreateSecondVenueSubmit) {
  TemporaryAgentFiles files;
  ASSERT_TRUE(files.Created());
  int unavailable_errno = 0;
  if (!UdsSocketsAvailable(files.Socket(), unavailable_errno)) {
    GTEST_SKIP() << "Unix SOCK_SEQPACKET unavailable: " << std::strerror(unavailable_errno);
  }

  const auto first_agent = StartAgent(files, true);
  ASSERT_GT(first_agent, 0);
  AgentProcess first_process(first_agent);

  const auto initial_status = agent::GetStatusOverUds(files.Socket());
  ASSERT_TRUE(initial_status.has_value());
  EXPECT_EQ(initial_status->agent_state, agent::AgentState::kReady);
  EXPECT_FALSE(initial_status->kill_switch_active);
  EXPECT_EQ(initial_status->gate_state, agent::WireGateState::kReady);
  EXPECT_EQ(initial_status->durable_order_count, 0U);
  EXPECT_EQ(initial_status->active_reservation_count, 0U);
  const auto already_ready = agent::ResumeOverUds(files.Socket());
  ASSERT_TRUE(already_ready.has_value());
  EXPECT_EQ(already_ready->result, agent::WireResumeResult::kAlreadyReady);
  EXPECT_EQ(already_ready->status.agent_state, agent::AgentState::kReady);

  const auto first = agent::SubmitOverUds(files.Socket(), Intent());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->status, agent::ResponseStatus::kSubmitted);
  EXPECT_EQ(first->submit_outcome, agent::WireSubmitOutcome::kUnknown);

  const auto retry = agent::SubmitOverUds(files.Socket(), Intent());
  ASSERT_TRUE(retry.has_value());
  EXPECT_EQ(retry->status, agent::ResponseStatus::kRiskRejected);
  EXPECT_TRUE(agent::HasRejectReason(*retry, agent::WireRejectReason::kDuplicateClientOrderId));

  const auto status_after_retry = agent::GetStatusOverUds(files.Socket());
  ASSERT_TRUE(status_after_retry.has_value());
  EXPECT_EQ(status_after_retry->durable_order_count, 1U);
  EXPECT_EQ(status_after_retry->active_reservation_count, 1U);
  EXPECT_EQ(status_after_retry->total_reserved_exposure.micros, 1'000'000);

  auto venue = demo::PersistentFaultInjectingVenue::Open(files.Venue(), false);
  ASSERT_TRUE(venue.has_value());
  ASSERT_EQ(venue->SubmitCount(), 1U);
  ASSERT_TRUE(venue->SetQueryAvailable(false));
  venue.reset();

  first_process.Stop();

  const auto restarted_agent = StartAgent(files, false);
  ASSERT_GT(restarted_agent, 0);
  AgentProcess restarted_process(restarted_agent);

  const auto recovery_required = agent::GetStatusOverUds(files.Socket());
  ASSERT_TRUE(recovery_required.has_value());
  EXPECT_EQ(recovery_required->agent_state, agent::AgentState::kRecoveryRequired);
  EXPECT_TRUE(recovery_required->kill_switch_active);
  EXPECT_EQ(recovery_required->gate_state, agent::WireGateState::kReconciliationRequired);
  EXPECT_EQ(recovery_required->durable_order_count, 1U);

  const auto blocked_while_recovery_required = agent::SubmitOverUds(files.Socket(), Intent(1002));
  ASSERT_TRUE(blocked_while_recovery_required.has_value());
  EXPECT_EQ(blocked_while_recovery_required->status, agent::ResponseStatus::kRiskRejected);
  EXPECT_TRUE(agent::HasRejectReason(*blocked_while_recovery_required, agent::WireRejectReason::kReconciliationRequired));

  const auto failed_resume = agent::ResumeOverUds(files.Socket());
  ASSERT_TRUE(failed_resume.has_value());
  EXPECT_EQ(failed_resume->result, agent::WireResumeResult::kRecoveryFailed);
  EXPECT_EQ(failed_resume->status.agent_state, agent::AgentState::kRecoveryRequired);
  EXPECT_TRUE(failed_resume->status.kill_switch_active);

  auto venue_control = demo::PersistentFaultInjectingVenue::Open(files.Venue(), false);
  ASSERT_TRUE(venue_control.has_value());
  ASSERT_TRUE(venue_control->SetQueryAvailable(true));
  EXPECT_EQ(venue_control->SubmitCount(), 1U);
  venue_control.reset();

  sqlite3* runtime_lock{};
  const auto runtime_path = files.Runtime().string();
  ASSERT_EQ(sqlite3_open_v2(runtime_path.c_str(), &runtime_lock, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(runtime_lock, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);
  const auto persistence_failed_resume = agent::ResumeOverUds(files.Socket());
  ASSERT_TRUE(persistence_failed_resume.has_value());
  EXPECT_EQ(persistence_failed_resume->result, agent::WireResumeResult::kPersistenceFailed);
  EXPECT_EQ(persistence_failed_resume->status.agent_state, agent::AgentState::kRecoveryRequired);
  EXPECT_TRUE(persistence_failed_resume->status.kill_switch_active);
  ASSERT_EQ(sqlite3_exec(runtime_lock, "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(runtime_lock), SQLITE_OK);

  const auto resumed = agent::ResumeOverUds(files.Socket());
  ASSERT_TRUE(resumed.has_value());
  EXPECT_EQ(resumed->result, agent::WireResumeResult::kReady);
  EXPECT_EQ(resumed->status.agent_state, agent::AgentState::kReady);
  EXPECT_FALSE(resumed->status.kill_switch_active);

  const auto retry_after_resume = agent::SubmitOverUds(files.Socket(), Intent());
  ASSERT_TRUE(retry_after_resume.has_value());
  EXPECT_EQ(retry_after_resume->status, agent::ResponseStatus::kRiskRejected);
  EXPECT_TRUE(agent::HasRejectReason(*retry_after_resume, agent::WireRejectReason::kDuplicateClientOrderId));

  const auto killed = agent::KillOverUds(files.Socket());
  ASSERT_TRUE(killed.has_value());
  EXPECT_EQ(killed->agent_state, agent::AgentState::kKilled);
  EXPECT_TRUE(killed->kill_switch_active);
  const auto killed_again = agent::KillOverUds(files.Socket());
  ASSERT_TRUE(killed_again.has_value());
  EXPECT_EQ(killed_again->agent_state, agent::AgentState::kKilled);
  EXPECT_TRUE(killed_again->kill_switch_active);

  restarted_process.Stop();

  const auto killed_restart = StartAgent(files, false);
  ASSERT_GT(killed_restart, 0);
  AgentProcess killed_process(killed_restart);
  const auto durable_killed = agent::GetStatusOverUds(files.Socket());
  ASSERT_TRUE(durable_killed.has_value());
  EXPECT_EQ(durable_killed->agent_state, agent::AgentState::kKilled);
  EXPECT_TRUE(durable_killed->kill_switch_active);

  const auto after_kill = agent::SubmitOverUds(files.Socket(), Intent(1002));
  ASSERT_TRUE(after_kill.has_value());
  EXPECT_EQ(after_kill->status, agent::ResponseStatus::kRiskRejected);
  EXPECT_TRUE(agent::HasRejectReason(*after_kill, agent::WireRejectReason::kKillSwitchActive));

  const auto resume_after_restart = agent::ResumeOverUds(files.Socket());
  ASSERT_TRUE(resume_after_restart.has_value());
  EXPECT_EQ(resume_after_restart->result, agent::WireResumeResult::kReady);
  EXPECT_EQ(resume_after_restart->status.agent_state, agent::AgentState::kReady);

  killed_process.Stop();

  const auto final_restart = StartAgent(files, false);
  ASSERT_GT(final_restart, 0);
  AgentProcess final_process(final_restart);
  const auto fresh_recovery = agent::GetStatusOverUds(files.Socket());
  ASSERT_TRUE(fresh_recovery.has_value());
  EXPECT_EQ(fresh_recovery->agent_state, agent::AgentState::kReady);
  const auto final_retry = agent::SubmitOverUds(files.Socket(), Intent());
  ASSERT_TRUE(final_retry.has_value());
  EXPECT_TRUE(agent::HasRejectReason(*final_retry, agent::WireRejectReason::kDuplicateClientOrderId));

  auto venue_after_restart = demo::PersistentFaultInjectingVenue::Open(files.Venue(), false);
  ASSERT_TRUE(venue_after_restart.has_value());
  EXPECT_EQ(venue_after_restart->SubmitCount(), 1U);
  venue_after_restart.reset();
  final_process.Stop();
}

TEST(UdsClientTimeoutTest, AcceptedSubmitWithoutResponseHonorsOneBoundedDeadline) {
  TemporaryAgentFiles files;
  ASSERT_TRUE(files.Created());
  int unavailable_errno = 0;
  if (!UdsSocketsAvailable(files.Socket(), unavailable_errno)) {
    GTEST_SKIP() << "Unix SOCK_SEQPACKET unavailable: " << std::strerror(unavailable_errno);
  }

  const auto server = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(server, 0);
  sockaddr_un address{};
  const auto path = files.Socket().string();
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  ASSERT_EQ(::bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(server, 1), 0);
  int stop[2]{};
  ASSERT_EQ(::pipe(stop), 0);

  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::close(stop[1]);
    const auto client = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    std::array<std::byte, agent::kMaxRequestFrameSize> request{};
    const auto received = client >= 0 ? ::recv(client, request.data(), request.size(), MSG_TRUNC) : -1;
    pollfd descriptor{.fd = stop[0], .events = POLLIN, .revents = 0};
    const auto stopped = received > 0 && ::poll(&descriptor, 1, 2'000) > 0;
    if (client >= 0) {
      ::close(client);
    }
    ::close(stop[0]);
    ::close(server);
    ::_exit(stopped ? 0 : 1);
  }

  ::close(stop[0]);
  ::close(server);
  const auto started = std::chrono::steady_clock::now();
  const auto response = agent::SubmitOverUds(files.Socket(), Intent(), agent::UdsClientOptions{.timeout = std::chrono::milliseconds{50}});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const char stop_byte = 'S';
  EXPECT_EQ(::write(stop[1], &stop_byte, 1), 1);
  ::close(stop[1]);
  int status{};
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  ASSERT_FALSE(response.has_value());
  EXPECT_EQ(response.error(), agent::UdsClientError::kTimedOut);
  EXPECT_GE(elapsed, std::chrono::milliseconds{40});
  EXPECT_LT(elapsed, std::chrono::seconds{1});
}

}  // namespace
