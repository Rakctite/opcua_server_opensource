#ifndef OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_OPTIONS_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_OPTIONS_H_

#include <string>

#include "common/result.h"

namespace opcua {

Result<int> ParseApiPort(const std::string& value);

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_OPTIONS_H_
