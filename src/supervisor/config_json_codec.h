#ifndef OPCUA_SERVER_SRC_SUPERVISOR_CONFIG_JSON_CODEC_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_CONFIG_JSON_CODEC_H_

#include <string>

#include "common/result.h"
#include "config/mqtt_config.h"
#include "config/server_config.h"

namespace opcua {

Result<ServerConfig> ParseServerConfigJson(const std::string& input);
std::string ServerConfigToJson(const ServerConfig& config);
Result<MqttConfig> ParseMqttConfigJson(const std::string& input);
std::string MqttConfigToJson(const MqttConfig& config);
std::string JsonError(const std::string& message);

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_CONFIG_JSON_CODEC_H_
