#include "config/config_repository.h"
#include "supervisor/api_server.h"
#include "supervisor/process_controller.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
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
  auto api_status = api_server.Run("0.0.0.0", 8080);
  if (!api_status.ok()) {
    std::cerr << "API server failed: " << api_status.message() << "\n";
    return 1;
  }
  return 0;
}
