#include "supervisor/process_controller.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <csignal>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

#if defined(__linux__)
void ConsumeShutdownSignal(int) {}

bool TestImmediateStopDoesNotLoseSignal(const std::string& child_path) {
  struct sigaction consume_action {};
  consume_action.sa_handler = ConsumeShutdownSignal;
  sigemptyset(&consume_action.sa_mask);
  struct sigaction previous_action {};
  if (sigaction(SIGTERM, &consume_action, &previous_action) != 0) {
    std::cerr << "failed to install inherited SIGTERM handler\n";
    return false;
  }

  bool passed = true;
  for (int attempt = 0; attempt < 50; ++attempt) {
    opcua::ProcessController controller(child_path, {});
    const auto start_status = controller.Start();
    if (!start_status.ok()) {
      std::cerr << "failed to start immediate-stop child\n";
      passed = false;
      break;
    }

    const auto stop_started = std::chrono::steady_clock::now();
    const auto stop_status = controller.Stop(500ms);
    const auto stop_duration = std::chrono::steady_clock::now() - stop_started;
    if (!stop_status.ok() || stop_duration >= 400ms) {
      std::cerr << "immediate child stop reached fallback: "
                << std::chrono::duration_cast<std::chrono::milliseconds>(
                       stop_duration)
                       .count()
                << "ms\n";
      passed = false;
      break;
    }
  }

  if (sigaction(SIGTERM, &previous_action, nullptr) != 0) {
    std::cerr << "failed to restore SIGTERM handler\n";
    return false;
  }
  return passed;
}
#endif

bool TestPermanentShutdownGate(const std::string& child_path) {
  opcua::ProcessController controller(child_path, {});
  controller.RequestShutdown();

  const auto start_status = controller.Start();
  const auto restart_status = controller.Restart(100ms);
  const auto status = controller.status();
  if (start_status.ok() || restart_status.ok() ||
      start_status.message().find("shutting down") == std::string::npos ||
      restart_status.message().find("shutting down") == std::string::npos ||
      status.state != opcua::ProcessState::kStopped) {
    std::cerr << "shutdown gate did not permanently reject start/restart\n";
    return false;
  }
  return true;
}

#if defined(__linux__)
bool TestShutdownOverlapsRestart(const std::string& child_path) {
  char marker_template[] = "/tmp/opcua-restart-marker-XXXXXX";
  const int marker_fd = mkstemp(marker_template);
  if (marker_fd < 0) {
    std::cerr << "failed to create marker path\n";
    return false;
  }
  close(marker_fd);
  (void)std::remove(marker_template);

  opcua::ProcessController controller(child_path, {marker_template});
  const auto start_status = controller.Start();
  if (!start_status.ok()) {
    std::cerr << "failed to start stubborn child\n";
    return false;
  }

  const auto ready_deadline = std::chrono::steady_clock::now() + 2s;
  bool child_ready = false;
  do {
    std::ifstream marker(marker_template);
    std::string marker_state;
    marker >> marker_state;
    child_ready = marker_state == "ready";
    if (!child_ready) {
      std::this_thread::sleep_for(10ms);
    }
  } while (!child_ready && std::chrono::steady_clock::now() < ready_deadline);
  if (!child_ready) {
    (void)controller.Stop(100ms);
    (void)std::remove(marker_template);
    std::cerr << "stubborn child did not become ready\n";
    return false;
  }

  std::packaged_task<opcua::Status()> restart_task(
      [&controller] { return controller.Restart(500ms); });
  auto restart_result = restart_task.get_future();
  std::thread restart_thread(std::move(restart_task));

  const auto marker_deadline = std::chrono::steady_clock::now() + 2s;
  bool term_observed = false;
  do {
    std::ifstream marker(marker_template);
    std::string marker_state;
    marker >> marker_state;
    term_observed = marker_state == "term";
    if (!term_observed) {
      std::this_thread::sleep_for(10ms);
    }
  } while (!term_observed &&
           std::chrono::steady_clock::now() < marker_deadline);

  if (!term_observed) {
    restart_thread.join();
    (void)std::remove(marker_template);
    std::cerr << "restart did not enter graceful stop\n";
    return false;
  }

  controller.RequestShutdown();
  restart_thread.join();
  const auto restart_status = restart_result.get();
  const auto status = controller.status();
  (void)std::remove(marker_template);
  if (restart_status.ok() ||
      restart_status.message().find("shutting down") == std::string::npos ||
      status.state != opcua::ProcessState::kStopped) {
    std::cerr << "overlapping restart spawned after shutdown request\n";
    return false;
  }
  return true;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "missing child path\n";
    return 1;
  }
#if defined(__linux__)
  if (!TestImmediateStopDoesNotLoseSignal(argv[1])) {
    return 1;
  }
#endif
  if (!TestPermanentShutdownGate(argv[1])) {
    return 1;
  }
#if defined(__linux__)
  if (argc < 3 || !TestShutdownOverlapsRestart(argv[2])) {
    return 1;
  }
#endif
  return 0;
}
