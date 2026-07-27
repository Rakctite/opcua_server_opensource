#ifndef OPCUA_SERVER_SRC_SUPERVISOR_CONFIG_JSON_CODEC_INTERNAL_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_CONFIG_JSON_CODEC_INTERNAL_H_

#include <string>

namespace opcua::internal {

std::string EscapeJsonString(const std::string& value);

}  // namespace opcua::internal

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_CONFIG_JSON_CODEC_INTERNAL_H_
