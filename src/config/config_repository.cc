#include "config/config_repository.h"

#include <charconv>
#include <cstdint>
#include <limits>
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
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return Status::Error("sqlite3_bind_text value exceeds maximum byte length");
  }
  const int byte_length = static_cast<int>(value.size());
  const int rc =
      sqlite3_bind_text(stmt, index, value.data(), byte_length, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return Status::Error(std::string("sqlite3_bind_text failed: ") + sqlite3_errmsg(db));
  }
  return Status::Ok();
}

Status BindInteger(sqlite3* db, sqlite3_stmt* stmt, int index,
                   sqlite3_int64 value) {
  const int rc = sqlite3_bind_int64(stmt, index, value);
  if (rc != SQLITE_OK) {
    return Status::Error(std::string("sqlite3_bind_int64 failed: ") +
                         sqlite3_errmsg(db));
  }
  return Status::Ok();
}

template <typename Bind>
Status ExecuteWrite(sqlite3* db, const char* sql, Bind bind) {
  auto stmt_result = Statement::Prepare(db, sql);
  if (!stmt_result.ok()) {
    return stmt_result.status();
  }

  auto& stmt = stmt_result.value();
  auto bind_status = bind(stmt.get());
  if (!bind_status.ok()) {
    return bind_status;
  }
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return Status::Error(std::string("sqlite3_step write failed: ") +
                         sqlite3_errmsg(db));
  }
  return Status::Ok();
}

Status RollbackAndReturn(SqliteDb* db, const Status& status) {
  const auto rollback_status = db->Execute("ROLLBACK;");
  if (!rollback_status.ok()) {
    return Status::Error(status.message() + "; rollback failed: " +
                         rollback_status.message());
  }
  return status;
}

Status InsertMqttDefaults(sqlite3* db, const MqttConfig& config) {
  auto source_status = ExecuteWrite(
      db,
      "INSERT OR IGNORE INTO mqtt_sources("
      "source_id, enabled, broker_uri, client_id) VALUES(1, ?, ?, ?);",
      [db, &config](sqlite3_stmt* stmt) {
        if (auto status = BindInteger(db, stmt, 1, config.enabled ? 1 : 0);
            !status.ok()) {
          return status;
        }
        if (auto status = BindText(db, stmt, 2, config.broker_uri);
            !status.ok()) {
          return status;
        }
        return BindText(db, stmt, 3, config.client_id);
      });
  if (!source_status.ok()) {
    return source_status;
  }

  auto node_status = ExecuteWrite(
      db,
      "INSERT OR IGNORE INTO data_nodes("
      "node_id, browse_name, data_type, stale_timeout_ms) "
      "SELECT ?, ?, ?, ? "
      "WHERE NOT EXISTS("
      "SELECT 1 FROM mqtt_mappings WHERE source_id=1);",
      [db, &config](sqlite3_stmt* stmt) {
        if (auto status = BindInteger(db, stmt, 1, config.node_id);
            !status.ok()) {
          return status;
        }
        if (auto status = BindText(db, stmt, 2, config.browse_name);
            !status.ok()) {
          return status;
        }
        if (auto status = BindText(db, stmt, 3, config.data_type);
            !status.ok()) {
          return status;
        }
        return BindInteger(db, stmt, 4, config.stale_timeout_ms);
      });
  if (!node_status.ok()) {
    return node_status;
  }

  return ExecuteWrite(
      db,
      "INSERT OR IGNORE INTO mqtt_mappings(source_id, topic, qos, node_id) "
      "SELECT 1, ?, ?, ? "
      "WHERE NOT EXISTS("
      "SELECT 1 FROM mqtt_mappings WHERE source_id=1);",
      [db, &config](sqlite3_stmt* stmt) {
        if (auto status = BindText(db, stmt, 1, config.topic); !status.ok()) {
          return status;
        }
        if (auto status = BindInteger(db, stmt, 2, config.qos); !status.ok()) {
          return status;
        }
        return BindInteger(db, stmt, 3, config.node_id);
      });
}

Result<std::string> ReadTextColumn(sqlite3_stmt* stmt, int column,
                                   const char* field_name) {
  if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
    return Status::Error(std::string("mqtt config field is not text: ") +
                         field_name);
  }
  const unsigned char* value = sqlite3_column_text(stmt, column);
  if (value == nullptr) {
    return Status::Error(std::string("mqtt config field is null: ") +
                         field_name);
  }
  const int length = sqlite3_column_bytes(stmt, column);
  return std::string(reinterpret_cast<const char*>(value),
                     static_cast<std::size_t>(length));
}

Result<sqlite3_int64> ReadIntegerColumn(sqlite3_stmt* stmt, int column,
                                        const char* field_name) {
  if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
    return Status::Error(std::string("mqtt config field is not integer: ") +
                         field_name);
  }
  return sqlite3_column_int64(stmt, column);
}

Result<int> NarrowToInt(sqlite3_int64 value, const char* field_name) {
  if (value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    return Status::Error(std::string("mqtt config integer out of range: ") +
                         field_name);
  }
  return static_cast<int>(value);
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
  auto begin_status = db_.Execute("BEGIN IMMEDIATE;");
  if (!begin_status.ok()) {
    return begin_status;
  }

  auto create_status = db_.Execute(
      "CREATE TABLE IF NOT EXISTS config("
      "key TEXT PRIMARY KEY, "
      "value TEXT NOT NULL);");
  if (!create_status.ok()) {
    return RollbackAndReturn(&db_, create_status);
  }

  create_status = db_.Execute(
      "CREATE TABLE IF NOT EXISTS mqtt_sources("
      "source_id INTEGER PRIMARY KEY CHECK(source_id = 1),"
      "enabled INTEGER NOT NULL CHECK(enabled IN (0, 1)),"
      "broker_uri TEXT NOT NULL,"
      "client_id TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS data_nodes("
      "node_id INTEGER PRIMARY KEY,"
      "browse_name TEXT NOT NULL,"
      "data_type TEXT NOT NULL,"
      "stale_timeout_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS mqtt_mappings("
      "source_id INTEGER NOT NULL REFERENCES mqtt_sources(source_id),"
      "topic TEXT NOT NULL,"
      "qos INTEGER NOT NULL,"
      "node_id INTEGER NOT NULL REFERENCES data_nodes(node_id),"
      "PRIMARY KEY(source_id, topic));");
  if (!create_status.ok()) {
    return RollbackAndReturn(&db_, create_status);
  }

  auto server_defaults_status =
      InsertDefaults(db_.get(), ConfigEntriesFrom(ServerConfig::Default()));
  if (!server_defaults_status.ok()) {
    return RollbackAndReturn(&db_, server_defaults_status);
  }

  auto mqtt_defaults_status =
      InsertMqttDefaults(db_.get(), MqttConfig::Default());
  if (!mqtt_defaults_status.ok()) {
    return RollbackAndReturn(&db_, mqtt_defaults_status);
  }

  auto commit_status = db_.Execute("COMMIT;");
  if (!commit_status.ok()) {
    return RollbackAndReturn(&db_, commit_status);
  }
  return Status::Ok();
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

Result<MqttConfig> ConfigRepository::LoadMqtt() {
  auto stmt_result =
      Statement::Prepare(db_.get(),
                         "SELECT s.enabled, s.broker_uri, s.client_id, "
                         "m.topic, m.qos, n.node_id, n.browse_name, "
                         "n.data_type, n.stale_timeout_ms "
                         "FROM mqtt_sources AS s "
                         "JOIN mqtt_mappings AS m ON m.source_id = s.source_id "
                         "JOIN data_nodes AS n ON n.node_id = m.node_id "
                         "WHERE s.source_id = 1;");
  if (!stmt_result.ok()) {
    return stmt_result.status();
  }

  auto& stmt = stmt_result.value();
  int step_rc = sqlite3_step(stmt.get());
  if (step_rc == SQLITE_DONE) {
    return Status::Error("MQTT config query returned no rows");
  }
  if (step_rc != SQLITE_ROW) {
    return Status::Error(std::string("sqlite3_step MQTT select failed: ") +
                         sqlite3_errmsg(db_.get()));
  }

  auto enabled_result = ReadIntegerColumn(stmt.get(), 0, "enabled");
  if (!enabled_result.ok()) {
    return enabled_result.status();
  }
  if (enabled_result.value() != 0 && enabled_result.value() != 1) {
    return Status::Error("mqtt config enabled must be 0 or 1");
  }
  auto broker_uri_result = ReadTextColumn(stmt.get(), 1, "broker_uri");
  if (!broker_uri_result.ok()) {
    return broker_uri_result.status();
  }
  auto client_id_result = ReadTextColumn(stmt.get(), 2, "client_id");
  if (!client_id_result.ok()) {
    return client_id_result.status();
  }
  auto topic_result = ReadTextColumn(stmt.get(), 3, "topic");
  if (!topic_result.ok()) {
    return topic_result.status();
  }
  auto qos_integer_result = ReadIntegerColumn(stmt.get(), 4, "qos");
  if (!qos_integer_result.ok()) {
    return qos_integer_result.status();
  }
  auto qos_result = NarrowToInt(qos_integer_result.value(), "qos");
  if (!qos_result.ok()) {
    return qos_result.status();
  }
  auto node_id_result = ReadIntegerColumn(stmt.get(), 5, "node_id");
  if (!node_id_result.ok()) {
    return node_id_result.status();
  }
  if (node_id_result.value() < 0 ||
      static_cast<std::uint64_t>(node_id_result.value()) >
          std::numeric_limits<std::uint32_t>::max()) {
    return Status::Error("mqtt config integer out of range: node_id");
  }
  auto browse_name_result = ReadTextColumn(stmt.get(), 6, "browse_name");
  if (!browse_name_result.ok()) {
    return browse_name_result.status();
  }
  auto data_type_result = ReadTextColumn(stmt.get(), 7, "data_type");
  if (!data_type_result.ok()) {
    return data_type_result.status();
  }
  auto timeout_integer_result =
      ReadIntegerColumn(stmt.get(), 8, "stale_timeout_ms");
  if (!timeout_integer_result.ok()) {
    return timeout_integer_result.status();
  }
  auto timeout_result =
      NarrowToInt(timeout_integer_result.value(), "stale_timeout_ms");
  if (!timeout_result.ok()) {
    return timeout_result.status();
  }

  MqttConfig config{enabled_result.value() == 1,
                    broker_uri_result.value(),
                    client_id_result.value(),
                    topic_result.value(),
                    qos_result.value(),
                    static_cast<std::uint32_t>(node_id_result.value()),
                    browse_name_result.value(),
                    data_type_result.value(),
                    timeout_result.value()};

  step_rc = sqlite3_step(stmt.get());
  if (step_rc == SQLITE_ROW) {
    return Status::Error("MQTT config query returned more than one row");
  }
  if (step_rc != SQLITE_DONE) {
    return Status::Error(std::string("sqlite3_step MQTT select failed: ") +
                         sqlite3_errmsg(db_.get()));
  }

  auto validate_status = config.Validate();
  if (!validate_status.ok()) {
    return validate_status;
  }
  return config;
}

Status ConfigRepository::SaveMqtt(const MqttConfig& config) {
  auto validate_status = config.Validate();
  if (!validate_status.ok()) {
    return validate_status;
  }

  auto begin_status = db_.Execute("BEGIN IMMEDIATE;");
  if (!begin_status.ok()) {
    return begin_status;
  }

  auto source_status = ExecuteWrite(
      db_.get(),
      "INSERT INTO mqtt_sources(source_id, enabled, broker_uri, client_id) "
      "VALUES(1, ?, ?, ?) "
      "ON CONFLICT(source_id) DO UPDATE SET enabled=excluded.enabled, "
      "broker_uri=excluded.broker_uri, client_id=excluded.client_id;",
      [this, &config](sqlite3_stmt* stmt) {
        if (auto status =
                BindInteger(db_.get(), stmt, 1, config.enabled ? 1 : 0);
            !status.ok()) {
          return status;
        }
        if (auto status = BindText(db_.get(), stmt, 2, config.broker_uri);
            !status.ok()) {
          return status;
        }
        return BindText(db_.get(), stmt, 3, config.client_id);
      });
  if (!source_status.ok()) {
    return RollbackAndReturn(&db_, source_status);
  }

  auto node_status = ExecuteWrite(
      db_.get(),
      "INSERT INTO data_nodes("
      "node_id, browse_name, data_type, stale_timeout_ms) VALUES(?, ?, ?, ?) "
      "ON CONFLICT(node_id) DO UPDATE SET browse_name=excluded.browse_name, "
      "data_type=excluded.data_type, "
      "stale_timeout_ms=excluded.stale_timeout_ms;",
      [this, &config](sqlite3_stmt* stmt) {
        if (auto status = BindInteger(db_.get(), stmt, 1, config.node_id);
            !status.ok()) {
          return status;
        }
        if (auto status = BindText(db_.get(), stmt, 2, config.browse_name);
            !status.ok()) {
          return status;
        }
        if (auto status = BindText(db_.get(), stmt, 3, config.data_type);
            !status.ok()) {
          return status;
        }
        return BindInteger(db_.get(), stmt, 4, config.stale_timeout_ms);
      });
  if (!node_status.ok()) {
    return RollbackAndReturn(&db_, node_status);
  }

  auto delete_mapping_status =
      ExecuteWrite(db_.get(), "DELETE FROM mqtt_mappings WHERE source_id=1;",
                   [](sqlite3_stmt*) { return Status::Ok(); });
  if (!delete_mapping_status.ok()) {
    return RollbackAndReturn(&db_, delete_mapping_status);
  }

  auto mapping_status = ExecuteWrite(
      db_.get(),
      "INSERT INTO mqtt_mappings(source_id, topic, qos, node_id) "
      "VALUES(1, ?, ?, ?);",
      [this, &config](sqlite3_stmt* stmt) {
        if (auto status = BindText(db_.get(), stmt, 1, config.topic);
            !status.ok()) {
          return status;
        }
        if (auto status = BindInteger(db_.get(), stmt, 2, config.qos);
            !status.ok()) {
          return status;
        }
        return BindInteger(db_.get(), stmt, 3, config.node_id);
      });
  if (!mapping_status.ok()) {
    return RollbackAndReturn(&db_, mapping_status);
  }

  auto cleanup_status =
      ExecuteWrite(db_.get(),
                   "DELETE FROM data_nodes WHERE NOT EXISTS("
                   "SELECT 1 FROM mqtt_mappings "
                   "WHERE mqtt_mappings.node_id=data_nodes.node_id);",
                   [](sqlite3_stmt*) { return Status::Ok(); });
  if (!cleanup_status.ok()) {
    return RollbackAndReturn(&db_, cleanup_status);
  }

  auto commit_status = db_.Execute("COMMIT;");
  if (!commit_status.ok()) {
    return RollbackAndReturn(&db_, commit_status);
  }
  return Status::Ok();
}

}  // namespace opcua
