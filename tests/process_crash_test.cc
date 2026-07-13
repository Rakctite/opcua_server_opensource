#include "supervisor/process_controller.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool WaitForState(opcua::ProcessController* controller,
                  opcua::ProcessState expected,
                  std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    controller->ReapExited();
    if (controller->status().state == expected) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return controller->status().state == expected;
}

bool TestUnexpectedExit(const std::string& crashing_child_path) {
  opcua::ProcessController controller(crashing_child_path, {});
  const auto start = controller.Start();
  if (!start.ok()) {
    std::cerr << "failed to start crashing child: " << start.message() << "\n";
    return false;
  }
  if (!WaitForState(&controller, opcua::ProcessState::kCrashed, 2s)) {
    std::cerr << "crashing child was not reaped\n";
    return false;
  }

  const auto status = controller.status();
  if (status.exit_code != 42) {
    std::cerr << "expected exit code 42, got " << status.exit_code << "\n";
    return false;
  }
  if (status.diagnostic.find("unexpected") == std::string::npos ||
      status.diagnostic.find("42") == std::string::npos) {
    std::cerr << "unexpected-exit diagnostic lost detail: "
              << status.diagnostic << "\n";
    return false;
  }
  return true;
}

bool TestExpectedStop(const std::string& test_child_path) {
  opcua::ProcessController controller(test_child_path, {});
  const auto start = controller.Start();
  if (!start.ok()) {
    std::cerr << "failed to start stoppable child: " << start.message() << "\n";
    return false;
  }
  const auto stop = controller.Stop(2s);
  if (!stop.ok()) {
    std::cerr << "failed to stop child: " << stop.message() << "\n";
    return false;
  }

  const auto status = controller.status();
  if (status.state != opcua::ProcessState::kStopped ||
      status.diagnostic != "process stopped") {
    std::cerr << "expected clean stopped status, got diagnostic: "
              << status.diagnostic << "\n";
    return false;
  }
  return true;
}

bool TestConcurrentStatusAndReaping(const std::string& test_child_path) {
  opcua::ProcessController controller(test_child_path, {});
  const auto start = controller.Start();
  if (!start.ok()) {
    std::cerr << "failed to start concurrent-access child: " << start.message()
              << "\n";
    return false;
  }

  std::atomic<bool> keep_polling{true};
  std::vector<std::thread> pollers;
  for (int i = 0; i < 4; ++i) {
    pollers.emplace_back([&controller, &keep_polling] {
      while (keep_polling.load()) {
        controller.ReapExited();
        (void)controller.status();
        std::this_thread::yield();
      }
    });
  }

  const auto restart = controller.Restart(2s);
  if (!restart.ok()) {
    keep_polling.store(false);
    for (auto& poller : pollers) {
      poller.join();
    }
    std::cerr << "concurrent restart failed: " << restart.message() << "\n";
    return false;
  }

  const auto stop = controller.Stop(2s);
  keep_polling.store(false);
  for (auto& poller : pollers) {
    poller.join();
  }
  if (!stop.ok()) {
    std::cerr << "concurrent stop failed: " << stop.message() << "\n";
    return false;
  }
  return controller.status().state == opcua::ProcessState::kStopped;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: process_crash_test <crashing-child> <test-child>\n";
    return 1;
  }

  if (!TestUnexpectedExit(argv[1]) || !TestExpectedStop(argv[2]) ||
      !TestConcurrentStatusAndReaping(argv[2])) {
    return 1;
  }
  return 0;
}
