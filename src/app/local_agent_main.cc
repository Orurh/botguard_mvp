#include "agent/uds_agent_server.h"
#include "demo/simulator_runtime.h"

#include <signal.h>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void Stop(int) noexcept {
  stop_requested = 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) {
    std::cerr << "Usage: " << argv[0] << " SOCKET_PATH ORDER_DB_PATH VENUE_DB_PATH [--lose-submit-response]\n";
    return 2;
  }
  const auto lose_response = argc == 5 && std::string_view(argv[4]) == "--lose-submit-response";
  if (argc == 5 && !lose_response) {
    std::cerr << "invalid option\n";
    return 2;
  }

  auto runtime = botguard::demo::SimulatorRuntime::Create({
      .order_database_path = argv[2],
      .venue_database_path = argv[3],
      .lose_submit_response = lose_response,
      .safety_inputs = nullptr,
      .crash_after_rollover_baseline_persisted = false,
  });
  if (!runtime) {
    std::cerr << "simulator runtime startup failed closed, error=" << static_cast<int>(runtime.error()) << '\n';
    return 1;
  }
  auto server = botguard::agent::UdsAgentServer::Create(std::filesystem::path(argv[1]), **runtime, true);
  if (!server) {
    std::cerr << "UDS server startup failed, error=" << static_cast<int>(server.error()) << '\n';
    return 1;
  }

  struct sigaction action{};
  action.sa_handler = Stop;
  static_cast<void>(::sigemptyset(&action.sa_mask));
  action.sa_flags = 0;
  if (::sigaction(SIGTERM, &action, nullptr) != 0 || ::sigaction(SIGINT, &action, nullptr) != 0) {
    std::cerr << "signal handler installation failed\n";
    return 1;
  }
  std::cout << "botguard_local_agent_ready socket=" << argv[1] << '\n' << std::flush;
  while (stop_requested == 0) {
    static_cast<void>(server->ServeOne());
  }
  return 0;
}
