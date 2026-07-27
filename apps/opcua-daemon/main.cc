#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "config/config_repository.h"
#include "daemon/opcua_server.h"

namespace {

volatile std::sig_atomic_t g_shutdown_requested = 0;

void HandleShutdownSignal(int /*signal*/) {
  g_shutdown_requested = 1;
}

int PrintError(const opcua::Status& status) {
  std::cerr << status.message() << "\n";
  return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGINT, HandleShutdownSignal);
  std::signal(SIGTERM, HandleShutdownSignal);

  const std::string db_path = argc > 1 ? argv[1] : "opcua-server.db";

  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (!repo_result.ok()) {
    return PrintError(repo_result.status());
  }

  auto& repo = repo_result.value();
  auto init_status = repo.Initialize();
  if (!init_status.ok()) {
    return PrintError(init_status);
  }

  auto config_result = repo.Load();
  if (!config_result.ok()) {
    return PrintError(config_result.status());
  }

  auto mqtt_config_result = repo.LoadMqtt();
  if (!mqtt_config_result.ok()) {
    return PrintError(mqtt_config_result.status());
  }

  opcua::OpcuaServer server(config_result.value(), mqtt_config_result.value());
  std::atomic_bool running(true);
  std::thread signal_monitor([&running]() {
    while (running.load() && g_shutdown_requested == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    running.store(false);
  });

  auto run_status = server.Run(&running);
  running.store(false);
  signal_monitor.join();
  if (!run_status.ok()) {
    return PrintError(run_status);
  }

  return 0;
}
