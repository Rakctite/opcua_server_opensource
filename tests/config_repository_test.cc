#include "config/config_repository.h"

#include <cstdio>
#include <iostream>
#include <string>

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

}  // namespace

int main() {
  const std::string db_path = "config_repository_test.db";
  RemoveDatabaseFiles(db_path);

  {
    auto repo_result = opcua::ConfigRepository::Open(db_path);
    if (int rc = Expect(repo_result.ok(), repo_result.status().message().c_str())) return rc;

    auto& repo = repo_result.value();
    auto init_status = repo.Initialize();
    if (int rc = Expect(init_status.ok(), init_status.message().c_str())) return rc;

    auto config_result = repo.Load();
    if (int rc = Expect(config_result.ok(), config_result.status().message().c_str())) return rc;
    if (int rc = Expect(config_result.value().server_port == 4840, "default port mismatch")) return rc;

    auto updated = config_result.value();
    updated.server_port = 4850;
    updated.server_bind_address = "127.0.0.1";
    auto save_status = repo.Save(updated);
    if (int rc = Expect(save_status.ok(), save_status.message().c_str())) return rc;

    auto loaded = repo.Load();
    if (int rc = Expect(loaded.ok(), loaded.status().message().c_str())) return rc;
    if (int rc = Expect(loaded.value().server_port == 4850, "updated port mismatch")) return rc;
    if (int rc = Expect(loaded.value().server_bind_address == "127.0.0.1",
                        "updated bind address mismatch")) {
      return rc;
    }
  }

  RemoveDatabaseFiles(db_path);
  return 0;
}
