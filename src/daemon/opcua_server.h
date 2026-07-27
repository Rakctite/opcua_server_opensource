#ifndef OPCUA_SERVER_SRC_DAEMON_OPCUA_SERVER_H_
#define OPCUA_SERVER_SRC_DAEMON_OPCUA_SERVER_H_

#include <atomic>

#include "common/result.h"
#include "config/mqtt_config.h"
#include "config/server_config.h"

namespace opcua {

class OpcuaServer {
 public:
  OpcuaServer(ServerConfig server_config, MqttConfig mqtt_config);
  Status Run(std::atomic_bool* running);

 private:
  ServerConfig server_config_;
  MqttConfig mqtt_config_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_DAEMON_OPCUA_SERVER_H_
