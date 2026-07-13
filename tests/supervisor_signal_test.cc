#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void RemoveDatabaseFiles(const std::string& path) {
  (void)std::remove(path.c_str());
  (void)std::remove((path + "-shm").c_str());
  (void)std::remove((path + "-wal").c_str());
}

pid_t ReadChildPid(pid_t supervisor_pid) {
  const std::string children_path =
      "/proc/" + std::to_string(supervisor_pid) + "/task/" +
      std::to_string(supervisor_pid) + "/children";
  std::ifstream children(children_path);
  pid_t child_pid = -1;
  children >> child_pid;
  return child_pid;
}

bool WaitForChild(pid_t supervisor_pid, pid_t* child_pid, int* wait_status) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t wait_result = waitpid(supervisor_pid, wait_status, WNOHANG);
    if (wait_result == supervisor_pid) {
      return false;
    }
    *child_pid = ReadChildPid(supervisor_pid);
    if (*child_pid > 0) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

bool WaitForExit(pid_t process_pid, int* wait_status) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t wait_result = waitpid(process_pid, wait_status, WNOHANG);
    if (wait_result == process_pid) {
      return true;
    }
    if (wait_result < 0 && errno != EINTR) {
      return false;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: supervisor_signal_test <supervisor> <daemon>\n";
    return 1;
  }

  char database_template[] = "/tmp/opcua-supervisor-signal-XXXXXX";
  const int database_fd = mkstemp(database_template);
  if (database_fd < 0) {
    std::cerr << "failed to create temporary database path\n";
    return 1;
  }
  close(database_fd);
  const std::string database_path(database_template);

  const pid_t supervisor_pid = fork();
  if (supervisor_pid < 0) {
    RemoveDatabaseFiles(database_path);
    std::cerr << "failed to fork supervisor\n";
    return 1;
  }
  if (supervisor_pid == 0) {
    execl(argv[1], argv[1], database_path.c_str(), argv[2], nullptr);
    _exit(127);
  }

  int wait_status = 0;
  pid_t daemon_pid = -1;
  if (!WaitForChild(supervisor_pid, &daemon_pid, &wait_status)) {
    (void)kill(supervisor_pid, SIGKILL);
    (void)waitpid(supervisor_pid, &wait_status, 0);
    RemoveDatabaseFiles(database_path);
    std::cerr << "supervisor daemon child did not start\n";
    return 1;
  }

  if (kill(supervisor_pid, SIGTERM) != 0 ||
      !WaitForExit(supervisor_pid, &wait_status)) {
    (void)kill(supervisor_pid, SIGKILL);
    (void)kill(daemon_pid, SIGKILL);
    (void)waitpid(supervisor_pid, &wait_status, 0);
    RemoveDatabaseFiles(database_path);
    std::cerr << "supervisor did not stop after SIGTERM\n";
    return 1;
  }

  const bool clean_exit = WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
  const bool daemon_reaped = kill(daemon_pid, 0) != 0 && errno == ESRCH;
  RemoveDatabaseFiles(database_path);
  if (!clean_exit) {
    std::cerr << "signal-triggered supervisor exit was not clean\n";
    return 1;
  }
  if (!daemon_reaped) {
    (void)kill(daemon_pid, SIGKILL);
    std::cerr << "daemon remained after supervisor exit\n";
    return 1;
  }
  return 0;
}
