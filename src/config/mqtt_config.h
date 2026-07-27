#ifndef OPCUA_SERVER_SRC_CONFIG_MQTT_CONFIG_H_
#define OPCUA_SERVER_SRC_CONFIG_MQTT_CONFIG_H_

#include <cstdint>
#include <string>

#include "common/result.h"

namespace opcua {

struct MqttConfig {
  bool enabled;
  std::string broker_uri;
  std::string client_id;
  std::string topic;
  int qos;
  std::uint32_t node_id;
  std::string browse_name;
  std::string data_type;
  int stale_timeout_ms;

  static MqttConfig Default();
  Status Validate() const;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_CONFIG_MQTT_CONFIG_H_
