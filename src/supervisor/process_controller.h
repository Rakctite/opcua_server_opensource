#ifndef OPCUA_SERVER_SRC_SUPERVISOR_PROCESS_CONTROLLER_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_PROCESS_CONTROLLER_H_

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "common/result.h"

namespace opcua {

enum class ProcessState {
  kStopped,
  kRunning,
  kCrashed,
};

struct ProcessStatus {
  ProcessState state = ProcessState::kStopped;
  int exit_code = 0;
  std::string diagnostic;
};

class ProcessController {
 public:
  ProcessController(std::string executable_path, std::vector<std::string> args);
  ~ProcessController();

  ProcessController(const ProcessController&) = delete;
  ProcessController& operator=(const ProcessController&) = delete;
  ProcessController(ProcessController&&) = delete;
  ProcessController& operator=(ProcessController&&) = delete;

  Status Start();
  Status Stop(std::chrono::milliseconds timeout);
  Status Restart(std::chrono::milliseconds timeout);
  void RequestShutdown();
  void ReapExited();
  ProcessStatus status();

 private:
  Status StartUnlocked();
  Status StopUnlocked(std::chrono::milliseconds timeout);
  void ReapExitedUnlocked();

#if defined(_WIN32)
  static std::wstring ToWideString(const std::string& value);
  static std::wstring BuildCommandLine(const std::string& executable_path,
                                       const std::vector<std::string>& args);
#endif

  std::string executable_path_;
  std::vector<std::string> args_;
  std::atomic<bool> shutdown_requested_{false};
  std::mutex mutex_;
  ProcessStatus status_;

#if defined(_WIN32)
  void* process_handle_ = nullptr;
#else
  int child_pid_ = -1;
#endif
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_PROCESS_CONTROLLER_H_
