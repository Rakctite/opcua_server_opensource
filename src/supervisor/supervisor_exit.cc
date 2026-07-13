#include "supervisor/supervisor_exit.h"

namespace opcua {

Status ClassifyApiExit(SupervisorExitReason reason, const Status& api_status) {
  if (reason == SupervisorExitReason::kApiExit && api_status.ok()) {
    return Status::Error("API server exited unexpectedly");
  }
  return api_status;
}

}  // namespace opcua
