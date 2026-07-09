#ifndef OPCUA_SERVER_SRC_CONFIG_SQLITE_DB_H_
#define OPCUA_SERVER_SRC_CONFIG_SQLITE_DB_H_

#include <string>

#include "common/result.h"
#include "sqlite3.h"

namespace opcua {

class SqliteDb {
 public:
  SqliteDb(const SqliteDb&) = delete;
  SqliteDb& operator=(const SqliteDb&) = delete;

  SqliteDb(SqliteDb&& other) noexcept;
  SqliteDb& operator=(SqliteDb&& other) noexcept;
  ~SqliteDb();

  static Result<SqliteDb> Open(const std::string& path);

  sqlite3* get() const { return db_; }
  Status Execute(const std::string& sql);

 private:
  explicit SqliteDb(sqlite3* db) : db_(db) {}

  sqlite3* db_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_CONFIG_SQLITE_DB_H_
