#include "config/config_repository.h"

#include <charconv>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace opcua {

namespace {

class Statement {
 public:
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
    other.stmt_ = nullptr;
  }

  Statement& operator=(Statement&& other) noexcept {
    if (this != &other) {
      Finalize();
      stmt_ = other.stmt_;
      other.stmt_ = nullptr;
    }
    return *this;
  }

  ~Statement() { Finalize(); }

  static Result<Statement> Prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      return Status::Error(std::string("sqlite3_prepare_v2 failed: ") + sqlite3_errmsg(db));
    }
    return Statement(stmt);
  }

  sqlite3_stmt* get() const { return stmt_; }

 private:
  explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}

  void Finalize() {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
      stmt_ = nullptr;
    }
  }

  sqlite3_stmt* stmt_;
};

struct ConfigEntry {
  const char* key;
  std::string value;
};

std::vector<ConfigEntry> ConfigEntriesFrom(const ServerConfig& config) {
  return {
      {"server.application_name", config.server_application_name},
      {"server.product_uri", config.server_product_uri},
      {"server.bind_address", config.server_bind_address},
      {"server.port", std::to_string(config.server_port)},
      {"server.endpoint_path", config.server_endpoint_path},
      {"security.mode", config.security_mode},
      {"security.policy", config.security_policy},
      {"limits.max_sessions", std::to_string(config.max_sessions)},
      {"limits.max_subscriptions", std::to_string(config.max_subscriptions)},
      {"logging.level", config.logging_level},
      {"logging.target", config.logging_target},
      {"address_space.mode", config.address_space_mode},
      {"address_space.path", config.address_space_path},
  };
}

Status BindText(sqlite3* db, sqlite3_stmt* stmt, int index, const std::string& value) {
  const int rc = sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return Status::Error(std::string("sqlite3_bind_text failed: ") + sqlite3_errmsg(db));
  }
  return Status::Ok();
}

Status WriteEntries(sqlite3* db, const std::vector<ConfigEntry>& entries, const char* sql) {
  auto stmt_result = Statement::Prepare(db, sql);
  if (!stmt_result.ok()) {
    return stmt_result.status();
  }

  auto& stmt = stmt_result.value();
  for (const auto& entry : entries) {
    sqlite3_reset(stmt.get());
    sqlite3_clear_bindings(stmt.get());

    auto key_status = BindText(db, stmt.get(), 1, entry.key);
    if (!key_status.ok()) {
      return key_status;
    }
    auto value_status = BindText(db, stmt.get(), 2, entry.value);
    if (!value_status.ok()) {
      return value_status;
    }

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
      return Status::Error(std::string("sqlite3_step write failed: ") + sqlite3_errmsg(db));
    }
  }

  return Status::Ok();
}

Status InsertDefaults(sqlite3* db, const std::vector<ConfigEntry>& entries) {
  return WriteEntries(db, entries, "INSERT OR IGNORE INTO config(key, value) VALUES(?, ?);");
}

Status UpsertEntries(sqlite3* db, const std::vector<ConfigEntry>& entries) {
  return WriteEntries(
      db, entries,
      "INSERT INTO config(key, value) VALUES(?, ?) "
      "ON CONFLICT(key) DO UPDATE SET value=excluded.value;");
}

Result<int> ParseIntValue(const std::unordered_map<std::string, std::string>& values,
                          const std::string& key) {
  const auto it = values.find(key);
  if (it == values.end()) {
    return Status::Error("missing config key: " + key);
  }

  const std::string& text = it->second;
  if (text.empty()) {
    return Status::Error("invalid integer config value for " + key + ": empty");
  }

  int value = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc() || result.ptr != end) {
    return Status::Error("invalid integer config value for " + key + ": " + text);
  }
  return value;
}

Result<std::string> StringValue(const std::unordered_map<std::string, std::string>& values,
                                const std::string& key) {
  const auto it = values.find(key);
  if (it == values.end()) {
    return Status::Error("missing config key: " + key);
  }
  return it->second;
}

Status ReadString(const std::unordered_map<std::string, std::string>& values,
                  const std::string& key, std::string* output) {
  auto result = StringValue(values, key);
  if (!result.ok()) {
    return result.status();
  }
  *output = result.value();
  return Status::Ok();
}

Status ReadInt(const std::unordered_map<std::string, std::string>& values,
               const std::string& key, int* output) {
  auto result = ParseIntValue(values, key);
  if (!result.ok()) {
    return result.status();
  }
  *output = result.value();
  return Status::Ok();
}

Status PopulateConfig(const std::unordered_map<std::string, std::string>& values,
                      ServerConfig* config) {
  if (auto status = ReadString(values, "server.application_name",
                               &config->server_application_name);
      !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "server.product_uri", &config->server_product_uri);
      !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "server.bind_address", &config->server_bind_address);
      !status.ok()) {
    return status;
  }
  if (auto status = ReadInt(values, "server.port", &config->server_port); !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "server.endpoint_path", &config->server_endpoint_path);
      !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "security.mode", &config->security_mode); !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "security.policy", &config->security_policy);
      !status.ok()) {
    return status;
  }
  if (auto status = ReadInt(values, "limits.max_sessions", &config->max_sessions);
      !status.ok()) {
    return status;
  }
  if (auto status = ReadInt(values, "limits.max_subscriptions", &config->max_subscriptions);
      !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "logging.level", &config->logging_level); !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "logging.target", &config->logging_target); !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "address_space.mode", &config->address_space_mode);
      !status.ok()) {
    return status;
  }
  if (auto status = ReadString(values, "address_space.path", &config->address_space_path);
      !status.ok()) {
    return status;
  }
  return Status::Ok();
}

}  // namespace

Result<ConfigRepository> ConfigRepository::Open(const std::string& db_path) {
  auto db_result = SqliteDb::Open(db_path);
  if (!db_result.ok()) {
    return db_result.status();
  }
  return ConfigRepository(std::move(db_result.value()));
}

Status ConfigRepository::Initialize() {
  auto create_status = db_.Execute(
      "CREATE TABLE IF NOT EXISTS config("
      "key TEXT PRIMARY KEY, "
      "value TEXT NOT NULL);");
  if (!create_status.ok()) {
    return create_status;
  }

  return InsertDefaults(db_.get(), ConfigEntriesFrom(ServerConfig::Default()));
}

Result<ServerConfig> ConfigRepository::Load() {
  auto stmt_result = Statement::Prepare(db_.get(), "SELECT key, value FROM config;");
  if (!stmt_result.ok()) {
    return stmt_result.status();
  }

  std::unordered_map<std::string, std::string> values;
  auto& stmt = stmt_result.value();
  while (true) {
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      return Status::Error(std::string("sqlite3_step select failed: ") + sqlite3_errmsg(db_.get()));
    }

    const unsigned char* key_text = sqlite3_column_text(stmt.get(), 0);
    const unsigned char* value_text = sqlite3_column_text(stmt.get(), 1);
    if (key_text == nullptr || value_text == nullptr) {
      return Status::Error("config row contained null key or value");
    }
    values.emplace(reinterpret_cast<const char*>(key_text),
                   reinterpret_cast<const char*>(value_text));
  }

  ServerConfig config;
  auto populate_status = PopulateConfig(values, &config);
  if (!populate_status.ok()) {
    return populate_status;
  }

  auto validate_status = config.Validate();
  if (!validate_status.ok()) {
    return validate_status;
  }

  return config;
}

Status ConfigRepository::Save(const ServerConfig& config) {
  auto validate_status = config.Validate();
  if (!validate_status.ok()) {
    return validate_status;
  }

  auto begin_status = db_.Execute("BEGIN IMMEDIATE;");
  if (!begin_status.ok()) {
    return begin_status;
  }

  auto write_status = UpsertEntries(db_.get(), ConfigEntriesFrom(config));
  if (!write_status.ok()) {
    db_.Execute("ROLLBACK;");
    return write_status;
  }

  auto commit_status = db_.Execute("COMMIT;");
  if (!commit_status.ok()) {
    db_.Execute("ROLLBACK;");
    return commit_status;
  }

  return Status::Ok();
}

}  // namespace opcua
