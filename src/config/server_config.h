#ifndef OPCUA_SERVER_SRC_CONFIG_SERVER_CONFIG_H_
#define OPCUA_SERVER_SRC_CONFIG_SERVER_CONFIG_H_

#include <string>

#include "common/result.h"

namespace opcua {

struct ServerConfig {
  std::string server_application_name;
  std::string server_product_uri;
  std::string server_bind_address;
  int server_port;
  std::string server_endpoint_path;
  std::string security_mode;
  std::string security_policy;
  int max_sessions;
  int max_subscriptions;
  std::string logging_level;
  std::string logging_target;
  std::string address_space_mode;
  std::string address_space_path;

  static ServerConfig Default();
  Status Validate() const;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_CONFIG_SERVER_CONFIG_H_
