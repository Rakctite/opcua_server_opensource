#include "config/config_repository.h"
#include "supervisor/api_server.h"
#include "supervisor/process_controller.h"
#include "supervisor/supervisor_exit.h"

#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t shutdown_signal = 0;

void HandleShutdownSignal(int signal) { shutdown_signal = signal; }

bool InstallSignalHandlers() {
  if (std::signal(SIGINT, HandleShutdownSignal) == SIG_ERR) {
    return false;
  }
#if defined(SIGTERM)
  if (std::signal(SIGTERM, HandleShutdownSignal) == SIG_ERR) {
    return false;
  }
#endif
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace std::chrono_literals;

  if (!InstallSignalHandlers()) {
    std::cerr << "failed to install supervisor signal handlers\n";
    return 1;
  }

  const std::string db_path = argc > 1 ? argv[1] : "opcua-server.db";
  const std::string daemon_path = argc > 2 ? argv[2] : "opcua-daemon";

  auto repository_result = opcua::ConfigRepository::Open(db_path);
  if (!repository_result.ok()) {
    std::cerr << "failed to open configuration repository: "
              << repository_result.status().message() << "\n";
    return 1;
  }
  auto repository = std::move(repository_result.value());
  auto initialize_status = repository.Initialize();
  if (!initialize_status.ok()) {
    std::cerr << "failed to initialize configuration repository: "
              << initialize_status.message() << "\n";
    return 1;
  }

  opcua::ProcessController controller(daemon_path, std::vector<std::string>{db_path});
  auto start_status = controller.Start();
  if (!start_status.ok()) {
    std::cerr << "failed to start daemon: " << start_status.message() << "\n";
  }

  opcua::ApiServer api_server(&repository, &controller);
  std::promise<opcua::Status> api_result_promise;
  auto api_result = api_result_promise.get_future();
  std::thread api_thread(
      [&api_server, promise = std::move(api_result_promise)]() mutable {
        promise.set_value(api_server.Run("0.0.0.0", 8080));
      });

  opcua::SupervisorExitReason exit_reason;
  while (true) {
    controller.ReapExited();
    if (shutdown_signal != 0) {
      exit_reason = opcua::SupervisorExitReason::kSignal;
      break;
    }
    if (api_result.wait_for(50ms) == std::future_status::ready) {
      exit_reason = opcua::SupervisorExitReason::kApiExit;
      break;
    }
  }

  api_server.Stop();
  const auto stop_status = controller.Stop(5000ms);
  controller.ReapExited();
  api_thread.join();
  const auto api_status = api_result.get();
  const auto supervisor_status =
      opcua::ClassifyApiExit(exit_reason, api_status);

  if (!stop_status.ok()) {
    std::cerr << "failed to stop daemon: " << stop_status.message() << "\n";
  }
  if (!supervisor_status.ok()) {
    std::cerr << "API server failed: " << supervisor_status.message() << "\n";
    return 1;
  }
  return stop_status.ok() ? 0 : 1;
}
