#ifndef OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_EXIT_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_EXIT_H_

#include "common/result.h"

namespace opcua {

enum class SupervisorExitReason {
  kSignal,
  kApiExit,
};

Status ClassifyApiExit(SupervisorExitReason reason, const Status& api_status);

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_EXIT_H_
