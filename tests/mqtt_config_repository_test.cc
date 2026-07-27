#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <utility>

#include "config/config_repository.h"
#include "config/mqtt_config.h"
#include "config/sqlite_db.h"

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

class TemporaryDatabase {
 public:
  TemporaryDatabase()
      : path_(
            (std::filesystem::temp_directory_path() /
             ("opcua_mqtt_config_repository_test_" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()) +
              ".db"))
                .string()) {
    RemoveDatabaseFiles(path_);
  }

  TemporaryDatabase(const TemporaryDatabase&) = delete;
  TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;

  ~TemporaryDatabase() { RemoveDatabaseFiles(path_); }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

bool ConfigsEqual(const opcua::MqttConfig& left,
                  const opcua::MqttConfig& right) {
  return left.enabled == right.enabled && left.broker_uri == right.broker_uri &&
         left.client_id == right.client_id && left.topic == right.topic &&
         left.qos == right.qos && left.node_id == right.node_id &&
         left.browse_name == right.browse_name &&
         left.data_type == right.data_type &&
         left.stale_timeout_ms == right.stale_timeout_ms;
}

int VerifySchema(const std::string& db_path) {
  auto db_result = opcua::SqliteDb::Open(db_path);
  if (int rc = Expect(db_result.ok(), db_result.status().message().c_str())) {
    return rc;
  }

  sqlite3_stmt* statement = nullptr;
  const char* sql =
      "SELECT name FROM sqlite_master "
      "WHERE type='table' AND name IN "
      "('config', 'mqtt_sources', 'data_nodes', 'mqtt_mappings');";
  const int prepare_rc =
      sqlite3_prepare_v2(db_result.value().get(), sql, -1, &statement, nullptr);
  if (int rc = Expect(prepare_rc == SQLITE_OK,
                      sqlite3_errmsg(db_result.value().get()))) {
    return rc;
  }

  std::set<std::string> tables;
  int step_rc = SQLITE_ROW;
  while ((step_rc = sqlite3_step(statement)) == SQLITE_ROW) {
    const unsigned char* name = sqlite3_column_text(statement, 0);
    if (name != nullptr) {
      tables.emplace(reinterpret_cast<const char*>(name));
    }
  }
  sqlite3_finalize(statement);

  if (int rc = Expect(step_rc == SQLITE_DONE,
                      sqlite3_errmsg(db_result.value().get()))) {
    return rc;
  }
  return Expect(
      tables == std::set<std::string>(
                    {"config", "data_nodes", "mqtt_mappings", "mqtt_sources"}),
      "expected config and normalized MQTT tables");
}

int ReadCount(const std::string& db_path, const char* sql, int* count) {
  auto db_result = opcua::SqliteDb::Open(db_path);
  if (int rc = Expect(db_result.ok(), db_result.status().message().c_str())) {
    return rc;
  }

  sqlite3_stmt* statement = nullptr;
  const int prepare_rc =
      sqlite3_prepare_v2(db_result.value().get(), sql, -1, &statement, nullptr);
  if (int rc = Expect(prepare_rc == SQLITE_OK,
                      sqlite3_errmsg(db_result.value().get()))) {
    return rc;
  }
  const int step_rc = sqlite3_step(statement);
  if (int rc = Expect(step_rc == SQLITE_ROW,
                      sqlite3_errmsg(db_result.value().get()))) {
    sqlite3_finalize(statement);
    return rc;
  }
  *count = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  return 0;
}

int ReadNodeCount(const std::string& db_path, std::uint32_t node_id,
                  int* count) {
  auto db_result = opcua::SqliteDb::Open(db_path);
  if (int rc = Expect(db_result.ok(), db_result.status().message().c_str())) {
    return rc;
  }

  sqlite3_stmt* statement = nullptr;
  const int prepare_rc =
      sqlite3_prepare_v2(db_result.value().get(),
                         "SELECT COUNT(*) FROM data_nodes WHERE node_id=?;", -1,
                         &statement, nullptr);
  if (int rc = Expect(prepare_rc == SQLITE_OK,
                      sqlite3_errmsg(db_result.value().get()))) {
    return rc;
  }
  if (sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(node_id)) !=
      SQLITE_OK) {
    sqlite3_finalize(statement);
    return Expect(false, "failed to bind node id");
  }

  const int step_rc = sqlite3_step(statement);
  if (int rc = Expect(step_rc == SQLITE_ROW,
                      sqlite3_errmsg(db_result.value().get()))) {
    sqlite3_finalize(statement);
    return rc;
  }
  *count = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  return 0;
}

int TestMqttPersistence(const std::string& db_path) {
  auto repository_result = opcua::ConfigRepository::Open(db_path);
  if (int rc = Expect(repository_result.ok(),
                      repository_result.status().message().c_str())) {
    return rc;
  }
  auto& repository = repository_result.value();

  auto initialize_status = repository.Initialize();
  if (int rc =
          Expect(initialize_status.ok(), initialize_status.message().c_str())) {
    return rc;
  }
  initialize_status = repository.Initialize();
  if (int rc =
          Expect(initialize_status.ok(), initialize_status.message().c_str())) {
    return rc;
  }

  auto default_result = repository.LoadMqtt();
  if (int rc = Expect(default_result.ok(),
                      default_result.status().message().c_str())) {
    return rc;
  }
  if (int rc = Expect(!default_result.value().enabled,
                      "default MQTT config should be disabled")) {
    return rc;
  }
  if (int rc = Expect(
          ConfigsEqual(default_result.value(), opcua::MqttConfig::Default()),
          "loaded MQTT defaults do not match MqttConfig::Default")) {
    return rc;
  }
  if (int rc = VerifySchema(db_path)) return rc;

  opcua::MqttConfig persisted{true,
                              "tcp://127.0.0.1:2883",
                              "repository-round-trip",
                              "factory/line-1/pressure",
                              1,
                              2001U,
                              "Line1Pressure",
                              "int64",
                              12345};
  auto save_status = repository.SaveMqtt(persisted);
  if (int rc = Expect(save_status.ok(), save_status.message().c_str())) {
    return rc;
  }

  auto loaded_result = repository.LoadMqtt();
  if (int rc = Expect(loaded_result.ok(),
                      loaded_result.status().message().c_str())) {
    return rc;
  }
  if (int rc = Expect(ConfigsEqual(loaded_result.value(), persisted),
                      "MQTT config did not round-trip all fields")) {
    return rc;
  }

  auto embedded_nul = persisted;
  embedded_nul.browse_name = std::string("Node\0Suffix", 11);
  save_status = repository.SaveMqtt(embedded_nul);
  if (int rc = Expect(!save_status.ok(),
                      "SaveMqtt should reject embedded NUL browse_name")) {
    return rc;
  }
  loaded_result = repository.LoadMqtt();
  if (int rc = Expect(loaded_result.ok(),
                      loaded_result.status().message().c_str())) {
    return rc;
  }
  if (int rc = Expect(ConfigsEqual(loaded_result.value(), persisted),
                      "embedded NUL SaveMqtt changed persisted config")) {
    return rc;
  }

  auto invalid = persisted;
  invalid.qos = 2;
  auto invalid_status = repository.SaveMqtt(invalid);
  if (int rc = Expect(!invalid_status.ok(), "SaveMqtt should reject qos=2")) {
    return rc;
  }
  loaded_result = repository.LoadMqtt();
  if (int rc = Expect(loaded_result.ok(),
                      loaded_result.status().message().c_str())) {
    return rc;
  }
  if (int rc = Expect(ConfigsEqual(loaded_result.value(), persisted),
                      "invalid SaveMqtt changed persisted config")) {
    return rc;
  }

  const std::uint32_t previous_node_id = persisted.node_id;
  auto replacement = persisted;
  replacement.topic = "factory/line-2/temperature";
  replacement.node_id = 3002U;
  replacement.browse_name = "Line2Temperature";
  replacement.data_type = "double";
  replacement.stale_timeout_ms = 6789;
  save_status = repository.SaveMqtt(replacement);
  if (int rc = Expect(save_status.ok(), save_status.message().c_str())) {
    return rc;
  }
  loaded_result = repository.LoadMqtt();
  if (int rc = Expect(loaded_result.ok(),
                      loaded_result.status().message().c_str())) {
    return rc;
  }
  if (int rc = Expect(ConfigsEqual(loaded_result.value(), replacement),
                      "replacement MQTT config did not round-trip")) {
    return rc;
  }

  int mapping_count = 0;
  if (int rc = ReadCount(db_path,
                         "SELECT COUNT(*) FROM mqtt_mappings "
                         "WHERE source_id=1;",
                         &mapping_count)) {
    return rc;
  }
  if (int rc = Expect(mapping_count == 1,
                      "replacement should leave exactly one source mapping")) {
    return rc;
  }

  int previous_node_count = 0;
  if (int rc = ReadNodeCount(db_path, previous_node_id, &previous_node_count)) {
    return rc;
  }
  return Expect(previous_node_count == 0,
                "replacement should delete the unreferenced previous node");
}

}  // namespace

int main() {
  TemporaryDatabase database;
  return TestMqttPersistence(database.path());
}
