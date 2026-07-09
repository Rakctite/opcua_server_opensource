#include "supervisor/process_controller.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "missing child path\n";
    return 1;
  }

  opcua::ProcessController controller(argv[1], {});
  auto start = controller.Start();
  if (!start.ok()) {
    std::cerr << start.message() << "\n";
    return 1;
  }
  if (controller.status().state != opcua::ProcessState::kRunning) {
    std::cerr << "expected running\n";
    return 1;
  }

  auto stop = controller.Stop(std::chrono::milliseconds(2000));
  if (!stop.ok()) {
    std::cerr << stop.message() << "\n";
    return 1;
  }
  if (controller.status().state != opcua::ProcessState::kStopped) {
    std::cerr << "expected stopped\n";
    return 1;
  }

  return 0;
}
