#include "config/config_repository.h"
#include "config/sqlite_db.h"

#include <algorithm>
#include <cctype>
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

int InitializeRepository(const std::string& db_path) {
  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (int rc = Expect(repo_result.ok(), repo_result.status().message().c_str())) return rc;

  auto init_status = repo_result.value().Initialize();
  if (int rc = Expect(init_status.ok(), init_status.message().c_str())) return rc;

  return 0;
}

int ExecuteSql(const std::string& db_path, const std::string& sql) {
  auto db_result = opcua::SqliteDb::Open(db_path);
  if (int rc = Expect(db_result.ok(), db_result.status().message().c_str())) return rc;

  auto status = db_result.value().Execute(sql);
  if (int rc = Expect(status.ok(), status.message().c_str())) return rc;

  return 0;
}

int ReadJournalMode(const std::string& db_path, std::string* journal_mode) {
  auto db_result = opcua::SqliteDb::Open(db_path);
  if (int rc = Expect(db_result.ok(), db_result.status().message().c_str())) return rc;

  sqlite3_stmt* stmt = nullptr;
  const int prepare_rc =
      sqlite3_prepare_v2(db_result.value().get(), "PRAGMA journal_mode;", -1, &stmt, nullptr);
  if (int rc = Expect(prepare_rc == SQLITE_OK, sqlite3_errmsg(db_result.value().get()))) {
    return rc;
  }

  const int step_rc = sqlite3_step(stmt);
  if (int rc = Expect(step_rc == SQLITE_ROW, sqlite3_errmsg(db_result.value().get()))) {
    sqlite3_finalize(stmt);
    return rc;
  }

  const unsigned char* text = sqlite3_column_text(stmt, 0);
  if (int rc = Expect(text != nullptr, "journal_mode returned null")) {
    sqlite3_finalize(stmt);
    return rc;
  }

  *journal_mode = reinterpret_cast<const char*>(text);
  sqlite3_finalize(stmt);
  return 0;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

int TestSaveAndLoadRoundTrip(const std::string& db_path) {
  if (int rc = InitializeRepository(db_path)) return rc;

  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (int rc = Expect(repo_result.ok(), repo_result.status().message().c_str())) return rc;

  auto& repo = repo_result.value();
  auto config_result = repo.Load();
  if (int rc = Expect(config_result.ok(), config_result.status().message().c_str())) return rc;
  if (int rc = Expect(config_result.value().server_port == 4840, "default port mismatch")) {
    return rc;
  }

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

  return 0;
}

int TestMissingKeyFailsLoad(const std::string& db_path) {
  if (int rc = InitializeRepository(db_path)) return rc;
  if (int rc = ExecuteSql(db_path, "DELETE FROM config WHERE key='server.port';")) return rc;

  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (int rc = Expect(repo_result.ok(), repo_result.status().message().c_str())) return rc;

  auto load_result = repo_result.value().Load();
  if (int rc = Expect(!load_result.ok(), "Load should fail when a required key is missing")) {
    return rc;
  }

  return 0;
}

int TestInvalidIntegerFailsLoad(const std::string& db_path) {
  if (int rc = InitializeRepository(db_path)) return rc;
  if (int rc = ExecuteSql(
          db_path,
          "UPDATE config SET value='not-an-int' WHERE key='server.port';")) {
    return rc;
  }

  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (int rc = Expect(repo_result.ok(), repo_result.status().message().c_str())) return rc;

  auto load_result = repo_result.value().Load();
  if (int rc = Expect(!load_result.ok(), "Load should fail for invalid integer values")) {
    return rc;
  }

  return 0;
}

int TestInvalidSavePreservesPreviousValues(const std::string& db_path) {
  if (int rc = InitializeRepository(db_path)) return rc;

  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (int rc = Expect(repo_result.ok(), repo_result.status().message().c_str())) return rc;

  auto& repo = repo_result.value();
  auto config_result = repo.Load();
  if (int rc = Expect(config_result.ok(), config_result.status().message().c_str())) return rc;

  auto updated = config_result.value();
  updated.server_port = 4850;
  auto save_status = repo.Save(updated);
  if (int rc = Expect(save_status.ok(), save_status.message().c_str())) return rc;

  auto invalid = updated;
  invalid.server_port = 70000;
  auto invalid_save_status = repo.Save(invalid);
  if (int rc = Expect(!invalid_save_status.ok(), "Save should reject invalid config")) return rc;

  auto loaded = repo.Load();
  if (int rc = Expect(loaded.ok(), loaded.status().message().c_str())) return rc;
  if (int rc = Expect(loaded.value().server_port == 4850,
                      "invalid Save should preserve previous port")) {
    return rc;
  }

  return 0;
}

int TestWalModeEnabled(const std::string& db_path) {
  if (int rc = InitializeRepository(db_path)) return rc;

  std::string journal_mode;
  if (int rc = ReadJournalMode(db_path, &journal_mode)) return rc;
  if (int rc = Expect(ToLower(journal_mode) == "wal", "journal_mode should be wal")) return rc;

  return 0;
}

}  // namespace

int main() {
  const std::string db_path = "config_repository_test.db";
  DatabaseCleanup cleanup(db_path);

  if (int rc = TestSaveAndLoadRoundTrip(db_path)) return rc;
  RemoveDatabaseFiles(db_path);

  if (int rc = TestMissingKeyFailsLoad(db_path)) return rc;
  RemoveDatabaseFiles(db_path);

  if (int rc = TestInvalidIntegerFailsLoad(db_path)) return rc;
  RemoveDatabaseFiles(db_path);

  if (int rc = TestInvalidSavePreservesPreviousValues(db_path)) return rc;
  RemoveDatabaseFiles(db_path);

  if (int rc = TestWalModeEnabled(db_path)) return rc;

  return 0;
}
