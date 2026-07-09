#include "config/sqlite_db.h"

#include <utility>

namespace opcua {

namespace {

std::string SqliteError(sqlite3* db, const std::string& action) {
  return action + ": " + sqlite3_errmsg(db);
}

}  // namespace

SqliteDb::SqliteDb(SqliteDb&& other) noexcept : db_(other.db_) {
  other.db_ = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept {
  if (this != &other) {
    if (db_ != nullptr) {
      sqlite3_close(db_);
    }
    db_ = other.db_;
    other.db_ = nullptr;
  }
  return *this;
}

SqliteDb::~SqliteDb() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
  }
}

Result<SqliteDb> SqliteDb::Open(const std::string& path) {
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    const std::string message = db == nullptr ? "sqlite3_open failed"
                                              : SqliteError(db, "sqlite3_open failed");
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return Status::Error(message);
  }

  SqliteDb sqlite_db(db);
  if (sqlite3_busy_timeout(sqlite_db.get(), 5000) != SQLITE_OK) {
    return Status::Error(SqliteError(sqlite_db.get(), "sqlite3_busy_timeout failed"));
  }

  auto wal_status = sqlite_db.Execute("PRAGMA journal_mode=WAL;");
  if (!wal_status.ok()) {
    return wal_status;
  }

  auto foreign_keys_status = sqlite_db.Execute("PRAGMA foreign_keys=ON;");
  if (!foreign_keys_status.ok()) {
    return foreign_keys_status;
  }

  return std::move(sqlite_db);
}

Status SqliteDb::Execute(const std::string& sql) {
  char* error_message = nullptr;
  const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error_message);
  if (rc != SQLITE_OK) {
    std::string message = error_message == nullptr ? sqlite3_errmsg(db_) : error_message;
    sqlite3_free(error_message);
    return Status::Error("sqlite3_exec failed: " + message);
  }
  return Status::Ok();
}

}  // namespace opcua
