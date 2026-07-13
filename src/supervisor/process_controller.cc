#include "supervisor/process_controller.h"

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdexcept>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace opcua {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(20);

#if defined(_WIN32)
constexpr DWORD kTerminatedExitCode = 1;

std::string LastErrorMessage(const char* context) {
  const DWORD error = GetLastError();
  std::ostringstream stream;
  stream << context << " failed with error " << error;
  return stream.str();
}

std::wstring QuoteWindowsArgument(const std::wstring& argument) {
  if (argument.empty()) {
    return L"\"\"";
  }

  const bool needs_quotes = argument.find_first_of(L" \t\n\v\"") !=
                            std::wstring::npos;
  if (!needs_quotes) {
    return argument;
  }

  std::wstring quoted = L"\"";
  int backslashes = 0;
  for (const wchar_t ch : argument) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'"') {
      quoted.append(static_cast<std::size_t>(backslashes * 2 + 1), L'\\');
      quoted.push_back(ch);
      backslashes = 0;
      continue;
    }
    quoted.append(static_cast<std::size_t>(backslashes), L'\\');
    quoted.push_back(ch);
    backslashes = 0;
  }
  quoted.append(static_cast<std::size_t>(backslashes * 2), L'\\');
  quoted.push_back(L'"');
  return quoted;
}
#else
std::string PosixErrorMessage(const char* context, int error) {
  std::ostringstream stream;
  stream << context << " failed: " << std::strerror(error);
  return stream.str();
}

std::string ErrnoMessage(const char* context) {
  return PosixErrorMessage(context, errno);
}

ProcessStatus DecodeExitedStatus(int wait_status, bool expected) {
  ProcessStatus status;
  if (WIFEXITED(wait_status)) {
    status.exit_code = WEXITSTATUS(wait_status);
    if (expected || status.exit_code == 0) {
      status.state = ProcessState::kStopped;
      status.diagnostic = "process stopped";
    } else {
      status.state = ProcessState::kCrashed;
      status.diagnostic = "process exited unexpectedly with code " +
                          std::to_string(status.exit_code);
    }
    return status;
  }

  if (WIFSIGNALED(wait_status)) {
    status.exit_code = WTERMSIG(wait_status);
    if (expected) {
      status.state = ProcessState::kStopped;
      status.diagnostic = "process stopped";
    } else {
      status.state = ProcessState::kCrashed;
      status.diagnostic =
          "process terminated unexpectedly by signal " +
          std::to_string(status.exit_code);
    }
    return status;
  }

  status.state = ProcessState::kCrashed;
  status.exit_code = wait_status;
  status.diagnostic = "process ended unexpectedly with unknown status " +
                      std::to_string(wait_status);
  return status;
}
#endif

}  // namespace

ProcessController::ProcessController(std::string executable_path,
                                     std::vector<std::string> args)
    : executable_path_(std::move(executable_path)), args_(std::move(args)) {}

ProcessController::~ProcessController() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state == ProcessState::kRunning) {
    (void)StopUnlocked(std::chrono::milliseconds(500));
  }
#if defined(_WIN32)
  if (process_handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(process_handle_));
    process_handle_ = nullptr;
  }
#endif
}

Status ProcessController::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  return StartUnlocked();
}

Status ProcessController::StartUnlocked() {
  if (shutdown_requested_.load()) {
    return Status::Error("process controller is shutting down");
  }
  ReapExitedUnlocked();
  if (status_.state == ProcessState::kRunning) {
    return Status::Error("process already running");
  }

#if defined(_WIN32)
  std::wstring application_path;
  std::wstring command_line;
  try {
    application_path = ToWideString(executable_path_);
    command_line = BuildCommandLine(executable_path_, args_);
  } catch (const std::runtime_error& error) {
    return Status::Error(error.what());
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info{};
  std::vector<wchar_t> mutable_command_line(command_line.begin(),
                                            command_line.end());
  mutable_command_line.push_back(L'\0');

  const BOOL created = CreateProcessW(
      application_path.c_str(), mutable_command_line.data(), nullptr, nullptr,
      FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info);
  if (created == FALSE) {
    return Status::Error(LastErrorMessage("CreateProcess"));
  }
  CloseHandle(process_info.hThread);
  process_handle_ = process_info.hProcess;
#else
  std::vector<char*> argv;
  argv.reserve(args_.size() + 2);
  argv.push_back(const_cast<char*>(executable_path_.c_str()));
  for (const auto& arg : args_) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  const char* executable_path = executable_path_.c_str();
  char* const* child_argv = argv.data();

  sigset_t shutdown_signals;
  if (sigemptyset(&shutdown_signals) != 0 ||
      sigaddset(&shutdown_signals, SIGTERM) != 0 ||
      sigaddset(&shutdown_signals, SIGINT) != 0) {
    return Status::Error(ErrnoMessage("prepare shutdown signal mask"));
  }

  struct sigaction default_action {};
  default_action.sa_handler = SIG_DFL;
  if (sigemptyset(&default_action.sa_mask) != 0) {
    return Status::Error(ErrnoMessage("prepare default signal action"));
  }

  sigset_t original_mask;
  const int block_error =
      pthread_sigmask(SIG_BLOCK, &shutdown_signals, &original_mask);
  if (block_error != 0) {
    return Status::Error(
        PosixErrorMessage("pthread_sigmask(SIG_BLOCK)", block_error));
  }

  const pid_t pid = fork();
  if (pid < 0) {
    const int fork_error = errno;
    const int restore_error =
        pthread_sigmask(SIG_SETMASK, &original_mask, nullptr);
    if (restore_error != 0) {
      return Status::Error(
          PosixErrorMessage("fork", fork_error) + "; " +
          PosixErrorMessage("pthread_sigmask(SIG_SETMASK)", restore_error));
    }
    return Status::Error(PosixErrorMessage("fork", fork_error));
  }
  if (pid == 0) {
    if (sigaction(SIGTERM, &default_action, nullptr) != 0 ||
        sigaction(SIGINT, &default_action, nullptr) != 0 ||
        sigprocmask(SIG_SETMASK, &original_mask, nullptr) != 0) {
      _exit(126);
    }
    execv(executable_path, child_argv);
    _exit(127);
  }

  const int restore_error =
      pthread_sigmask(SIG_SETMASK, &original_mask, nullptr);
  if (restore_error != 0) {
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
      child_pid_ = pid;
      status_.state = ProcessState::kRunning;
      status_.exit_code = 0;
      status_.diagnostic = "process running after signal mask restore failure";
      return Status::Error(
          PosixErrorMessage("pthread_sigmask(SIG_SETMASK)", restore_error));
    }

    int wait_status = 0;
    while (waitpid(pid, &wait_status, 0) < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECHILD) {
        break;
      }
      const int wait_error = errno;
      child_pid_ = pid;
      status_.state = ProcessState::kRunning;
      status_.exit_code = 0;
      status_.diagnostic = "process ownership retained after waitpid failure";
      return Status::Error(
          PosixErrorMessage("pthread_sigmask(SIG_SETMASK)", restore_error) +
          "; " + PosixErrorMessage("waitpid", wait_error));
    }
    return Status::Error(
        PosixErrorMessage("pthread_sigmask(SIG_SETMASK)", restore_error));
  }
  child_pid_ = pid;
#endif

  status_.state = ProcessState::kRunning;
  status_.exit_code = 0;
  status_.diagnostic = "process running";
  return Status::Ok();
}

Status ProcessController::Stop(std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lock(mutex_);
  return StopUnlocked(timeout);
}

Status ProcessController::StopUnlocked(std::chrono::milliseconds timeout) {
  ReapExitedUnlocked();
  if (status_.state != ProcessState::kRunning) {
    return Status::Ok();
  }

#if defined(_WIN32)
  const auto timeout_ms = timeout.count() < 0 ? 0 : timeout.count();
  if (TerminateProcess(static_cast<HANDLE>(process_handle_),
                       kTerminatedExitCode) == FALSE) {
    return Status::Error(LastErrorMessage("TerminateProcess"));
  }

  const DWORD wait_result =
      WaitForSingleObject(static_cast<HANDLE>(process_handle_),
                          static_cast<DWORD>(timeout_ms));
  if (wait_result == WAIT_TIMEOUT) {
    return Status::Error("process termination timed out");
  }
  if (wait_result == WAIT_FAILED) {
    return Status::Error(LastErrorMessage("WaitForSingleObject"));
  }

  CloseHandle(static_cast<HANDLE>(process_handle_));
  process_handle_ = nullptr;
#else
  if (kill(child_pid_, SIGTERM) != 0 && errno != ESRCH) {
    return Status::Error(ErrnoMessage("kill(SIGTERM)"));
  }

  int wait_status = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t result = waitpid(child_pid_, &wait_status, WNOHANG);
    if (result == child_pid_) {
      status_ = DecodeExitedStatus(wait_status, true);
      child_pid_ = -1;
      return Status::Ok();
    }
    if (result < 0) {
      if (errno == ECHILD) {
        child_pid_ = -1;
        status_.state = ProcessState::kStopped;
        status_.exit_code = 0;
        status_.diagnostic = "process stopped";
        return Status::Ok();
      }
      return Status::Error(ErrnoMessage("waitpid"));
    }
    std::this_thread::sleep_for(kPollInterval);
  }

  if (kill(child_pid_, SIGKILL) != 0 && errno != ESRCH) {
    return Status::Error(ErrnoMessage("kill(SIGKILL)"));
  }
  while (waitpid(child_pid_, &wait_status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    if (errno == ECHILD) {
      break;
    }
    return Status::Error(ErrnoMessage("waitpid"));
  }
  child_pid_ = -1;
#endif

  status_.state = ProcessState::kStopped;
  status_.exit_code = 0;
  status_.diagnostic = "process stopped";
  return Status::Ok();
}

Status ProcessController::Restart(std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_requested_.load()) {
    return Status::Error("process controller is shutting down");
  }
  auto stop_status = StopUnlocked(timeout);
  if (!stop_status.ok()) {
    return stop_status;
  }
  if (shutdown_requested_.load()) {
    return Status::Error("process controller is shutting down");
  }
  return StartUnlocked();
}

void ProcessController::RequestShutdown() { shutdown_requested_.store(true); }

void ProcessController::ReapExited() {
  std::lock_guard<std::mutex> lock(mutex_);
  ReapExitedUnlocked();
}

void ProcessController::ReapExitedUnlocked() {
  if (status_.state != ProcessState::kRunning) {
    return;
  }

#if defined(_WIN32)
  if (process_handle_ == nullptr) {
    return;
  }
  const DWORD wait_result =
      WaitForSingleObject(static_cast<HANDLE>(process_handle_), 0);
  if (wait_result == WAIT_TIMEOUT) {
    return;
  }
  if (wait_result == WAIT_FAILED) {
    status_.state = ProcessState::kCrashed;
    status_.exit_code = static_cast<int>(GetLastError());
    status_.diagnostic = "WaitForSingleObject failed while reaping process";
    CloseHandle(static_cast<HANDLE>(process_handle_));
    process_handle_ = nullptr;
    return;
  }

  DWORD exit_code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &exit_code) ==
      FALSE) {
    status_.state = ProcessState::kCrashed;
    status_.exit_code = static_cast<int>(GetLastError());
    status_.diagnostic = "GetExitCodeProcess failed";
    CloseHandle(static_cast<HANDLE>(process_handle_));
    process_handle_ = nullptr;
    return;
  }

  CloseHandle(static_cast<HANDLE>(process_handle_));
  process_handle_ = nullptr;
  status_.exit_code = static_cast<int>(exit_code);
  if (exit_code == 0) {
    status_.state = ProcessState::kStopped;
    status_.diagnostic = "process stopped";
  } else {
    status_.state = ProcessState::kCrashed;
    status_.diagnostic = "process exited unexpectedly with code " +
                         std::to_string(status_.exit_code);
  }
#else
  if (child_pid_ < 0) {
    return;
  }
  int wait_status = 0;
  const pid_t result = waitpid(child_pid_, &wait_status, WNOHANG);
  if (result == 0) {
    return;
  }
  if (result == child_pid_) {
    status_ = DecodeExitedStatus(wait_status, false);
    child_pid_ = -1;
    return;
  }
  if (result < 0 && errno == ECHILD) {
    child_pid_ = -1;
    status_.state = ProcessState::kStopped;
    status_.exit_code = 0;
    status_.diagnostic = "process stopped";
  }
#endif
}

ProcessStatus ProcessController::status() {
  std::lock_guard<std::mutex> lock(mutex_);
  ReapExitedUnlocked();
  return status_;
}

#if defined(_WIN32)
std::wstring ProcessController::ToWideString(const std::string& value) {
  if (value.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      value.data(),
                                      static_cast<int>(value.size()), nullptr,
                                      0);
  if (size <= 0) {
    throw std::runtime_error("failed to convert UTF-8 path to UTF-16");
  }
  std::wstring converted(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), converted.data(), size);
  return converted;
}

std::wstring ProcessController::BuildCommandLine(
    const std::string& executable_path, const std::vector<std::string>& args) {
  std::wstring command_line = QuoteWindowsArgument(ToWideString(executable_path));
  for (const auto& arg : args) {
    command_line.push_back(L' ');
    command_line += QuoteWindowsArgument(ToWideString(arg));
  }
  return command_line;
}
#endif

}  // namespace opcua
