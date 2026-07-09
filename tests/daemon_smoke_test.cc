#include "config/config_repository.h"
#include "daemon/opcua_server.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

int Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

void RemoveDatabaseFiles(const std::string& db_path) {
  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());
}

class DatabaseCleanup {
 public:
  explicit DatabaseCleanup(std::string db_path) : db_path_(std::move(db_path)) {
    RemoveDatabaseFiles(db_path_);
  }

  DatabaseCleanup(const DatabaseCleanup&) = delete;
  DatabaseCleanup& operator=(const DatabaseCleanup&) = delete;

  ~DatabaseCleanup() { RemoveDatabaseFiles(db_path_); }

 private:
  std::string db_path_;
};

opcua::Status RunServerBriefly(const opcua::ServerConfig& config) {
  opcua::OpcuaServer server(config);
  std::atomic_bool running(true);
  opcua::Status run_status = opcua::Status::Ok();
  std::thread server_thread([&server, &running, &run_status]() {
    run_status = server.Run(&running);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  running.store(false);
  server_thread.join();

  return run_status;
}

}  // namespace

int main() {
  const std::string db_path = "daemon_smoke_test.db";
  DatabaseCleanup cleanup(db_path);

  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (int rc = Expect(repo_result.ok(), repo_result.status().message().c_str())) return rc;

  auto& repo = repo_result.value();
  auto init_status = repo.Initialize();
  if (int rc = Expect(init_status.ok(), init_status.message().c_str())) return rc;

  auto config_result = repo.Load();
  if (int rc = Expect(config_result.ok(), config_result.status().message().c_str())) return rc;

  const std::array<int, 5> test_ports = {48400, 48401, 48402, 48403, 48404};
  opcua::Status last_run_status = opcua::Status::Error("server did not run");
  for (const int port : test_ports) {
    auto config = config_result.value();
    config.server_port = port;
    auto save_status = repo.Save(config);
    if (int rc = Expect(save_status.ok(), save_status.message().c_str())) return rc;

    auto loaded_config_result = repo.Load();
    if (int rc = Expect(loaded_config_result.ok(),
                        loaded_config_result.status().message().c_str())) {
      return rc;
    }
    if (int rc = Expect(loaded_config_result.value().server_port == port,
                        "daemon smoke config did not persist test port")) {
      return rc;
    }

    last_run_status = RunServerBriefly(loaded_config_result.value());
    if (last_run_status.ok()) {
      return 0;
    }
    std::cerr << "daemon smoke failed on port " << port << ": "
              << last_run_status.message() << "\n";
  }

  return Expect(false, last_run_status.message().c_str());
}
