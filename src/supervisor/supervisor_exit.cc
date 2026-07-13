#include "supervisor/supervisor_exit.h"

namespace opcua {

std::optional<SupervisorExitReason> ObserveSupervisorExit(
    bool api_ready, bool signal_pending, bool api_ready_after_signal) {
  if (api_ready || (signal_pending && api_ready_after_signal)) {
    return SupervisorExitReason::kApiExit;
  }
  if (signal_pending) {
    return SupervisorExitReason::kSignal;
  }
  return std::nullopt;
}

Status ClassifyApiExit(SupervisorExitReason reason, const Status& api_status) {
  if (reason == SupervisorExitReason::kApiExit && api_status.ok()) {
    return Status::Error("API server exited unexpectedly");
  }
  return api_status;
}

}  // namespace opcua
