#include <atomic>
#include <csignal>
#include <iostream>
#include <string>

#include "config/config_repository.h"
#include "daemon/opcua_server.h"

namespace {

std::atomic_bool g_running(true);

void HandleShutdownSignal(int /*signal*/) {
  g_running.store(false);
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

  opcua::OpcuaServer server(config_result.value());
  auto run_status = server.Run(&g_running);
  if (!run_status.ok()) {
    return PrintError(run_status);
  }

  return 0;
}
