#include "supervisor/process_controller.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  using namespace std::chrono_literals;

  if (argc < 2) {
    std::cerr << "missing exit-259 child path\n";
    return 1;
  }

  opcua::ProcessController controller(argv[1], {});
  const auto start_status = controller.Start();
  if (!start_status.ok()) {
    std::cerr << "failed to start exit-259 child: " << start_status.message()
              << "\n";
    return 1;
  }

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  opcua::ProcessStatus status;
  do {
    status = controller.status();
    if (status.state == opcua::ProcessState::kCrashed) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);

  if (status.state != opcua::ProcessState::kCrashed ||
      status.exit_code != 259 ||
      status.diagnostic.find("unexpected") == std::string::npos) {
    std::cerr << "exit code 259 was not reaped as an unexpected exit\n";
    return 1;
  }
  return 0;
}
