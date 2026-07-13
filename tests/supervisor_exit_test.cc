#include "supervisor/supervisor_exit.h"

#include <iostream>
#include <string>

int main() {
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
