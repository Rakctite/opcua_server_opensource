#include "supervisor/supervisor_exit.h"

#include <iostream>
#include <string>

int main() {
  const auto simultaneous_exit =
      opcua::ObserveSupervisorExit(true, true);
  if (!simultaneous_exit.has_value() ||
      simultaneous_exit.value() != opcua::SupervisorExitReason::kApiExit) {
    std::cerr << "ready API must take precedence over pending signal\n";
    return 1;
  }

  const auto api_exit = opcua::ObserveSupervisorExit(true, false);
  if (!api_exit.has_value() ||
      api_exit.value() != opcua::SupervisorExitReason::kApiExit) {
    std::cerr << "ready API should end supervision\n";
    return 1;
  }

  const auto signal_observed = opcua::ObserveSupervisorExit(false, true);
  if (!signal_observed.has_value() ||
      signal_observed.value() != opcua::SupervisorExitReason::kSignal) {
    std::cerr << "pending signal should end supervision\n";
    return 1;
  }

  if (opcua::ObserveSupervisorExit(false, false).has_value()) {
    std::cerr << "supervision should continue without an exit condition\n";
    return 1;
  }

  const auto signal_exit = opcua::ClassifyApiExit(
      opcua::SupervisorExitReason::kSignal, opcua::Status::Ok());
  if (!signal_exit.ok()) {
    std::cerr << "signal-triggered API stop should be clean\n";
    return 1;
  }

  const auto unsolicited_exit = opcua::ClassifyApiExit(
      opcua::SupervisorExitReason::kApiExit, opcua::Status::Ok());
  if (unsolicited_exit.ok() ||
      unsolicited_exit.message().find("unexpected") == std::string::npos) {
    std::cerr << "unsolicited successful API exit should fail supervision\n";
    return 1;
  }

  const auto api_error = opcua::ClassifyApiExit(
      opcua::SupervisorExitReason::kApiExit,
      opcua::Status::Error("listener failed"));
  if (api_error.ok() || api_error.message() != "listener failed") {
    std::cerr << "API failure detail should be preserved\n";
    return 1;
  }
  return 0;
}
