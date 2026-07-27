#ifndef OPCUA_SERVER_SRC_DAEMON_MQTT_PAYLOAD_PARSER_H_
#define OPCUA_SERVER_SRC_DAEMON_MQTT_PAYLOAD_PARSER_H_

#include <cstddef>
#include <string_view>

#include "common/result.h"
#include "daemon/realtime_value_store.h"

namespace opcua {

constexpr std::size_t kMaxMqttScalarPayloadBytes = 128;

Result<ScalarValue> ParseMqttScalar(std::string_view payload, ScalarType type);

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_DAEMON_MQTT_PAYLOAD_PARSER_H_
