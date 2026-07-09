#include "config/config_repository.h"
#include "daemon/opcua_server.h"

#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>
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

  opcua::OpcuaServer server(config_result.value());
  std::atomic_bool running(false);
  auto run_status = server.Run(&running);
  if (int rc = Expect(run_status.ok(), run_status.message().c_str())) return rc;

  return 0;
}
