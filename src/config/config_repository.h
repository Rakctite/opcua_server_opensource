#ifndef OPCUA_SERVER_SRC_CONFIG_CONFIG_REPOSITORY_H_
#define OPCUA_SERVER_SRC_CONFIG_CONFIG_REPOSITORY_H_

#include <string>
#include <utility>

#include "common/result.h"
#include "config/server_config.h"
#include "config/sqlite_db.h"

namespace opcua {

class ConfigRepository {
 public:
  ConfigRepository(const ConfigRepository&) = delete;
  ConfigRepository& operator=(const ConfigRepository&) = delete;
  ConfigRepository(ConfigRepository&&) noexcept = default;
  ConfigRepository& operator=(ConfigRepository&&) noexcept = default;

  static Result<ConfigRepository> Open(const std::string& db_path);

  Status Initialize();
  Result<ServerConfig> Load();
  Status Save(const ServerConfig& config);

 private:
  explicit ConfigRepository(SqliteDb db) : db_(std::move(db)) {}

  SqliteDb db_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_CONFIG_CONFIG_REPOSITORY_H_
