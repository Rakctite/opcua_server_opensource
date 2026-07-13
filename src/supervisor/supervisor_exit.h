#ifndef OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_EXIT_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_EXIT_H_

#include <future>
#include <optional>

#include "common/result.h"

namespace opcua {

enum class SupervisorExitReason {
  kSignal,
  kApiExit,
};

std::optional<SupervisorExitReason> ObserveSupervisorExit(
    bool api_ready, bool signal_pending, bool api_ready_after_signal);
Status ClassifyApiExit(SupervisorExitReason reason, const Status& api_status);
Status GetApiResult(std::future<Status>* api_result);

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_SUPERVISOR_EXIT_H_
