#ifndef OPCUA_SERVER_SRC_SUPERVISOR_API_SERVER_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_API_SERVER_H_

#include <memory>
#include <string>

#include "common/result.h"

namespace opcua {

class ConfigRepository;
class ProcessController;

class ApiServer {
 public:
  ApiServer(ConfigRepository* repository, ProcessController* controller);
  ~ApiServer();

  ApiServer(const ApiServer&) = delete;
  ApiServer& operator=(const ApiServer&) = delete;
  ApiServer(ApiServer&&) = delete;
  ApiServer& operator=(ApiServer&&) = delete;

  Status Run(const std::string& bind_address, int port);
  void Stop();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_API_SERVER_H_
