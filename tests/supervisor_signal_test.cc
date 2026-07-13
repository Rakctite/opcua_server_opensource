#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/prctl.h>
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

class DatabaseCleanup {
 public:
  explicit DatabaseCleanup(std::string path) : path_(std::move(path)) {}
  ~DatabaseCleanup() { RemoveDatabaseFiles(path_); }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

class DefaultPortCollision {
 public:
  DefaultPortCollision() {
    socket_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_ < 0) {
      return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8080);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
        0) {
      valid_ = listen(socket_, 1) == 0;
      return;
    }
    if (errno == EADDRINUSE) {
      close(socket_);
      socket_ = -1;
      valid_ = true;
    }
  }

  ~DefaultPortCollision() {
    if (socket_ >= 0) {
      close(socket_);
    }
  }

  bool valid() const { return valid_; }

 private:
  int socket_ = -1;
  bool valid_ = false;
};

class SupervisorProcess {
 public:
  enum class PollResult { kRunning, kExited, kOwnershipError };

  explicit SupervisorProcess(pid_t pid) : pid_(pid) {}
  ~SupervisorProcess() { Cleanup(); }

  pid_t pid() const { return pid_; }
  int wait_status() const { return wait_status_; }
  void RecordChild(pid_t child_pid) { recorded_child_pid_ = child_pid; }

  PollResult Poll() {
    if (reaped_) {
      return PollResult::kExited;
    }
    if (ownership_error_) {
      return PollResult::kOwnershipError;
    }
    const pid_t result = waitpid(pid_, &wait_status_, WNOHANG);
    if (result == pid_) {
      reaped_ = true;
      return PollResult::kExited;
    }
    if (result < 0 && errno == ECHILD) {
      ownership_error_ = true;
      return PollResult::kOwnershipError;
    }
    if (result < 0 && errno != EINTR) {
      ownership_error_ = true;
      return PollResult::kOwnershipError;
    }
    return PollResult::kRunning;
  }

  bool WaitForExit(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const PollResult result = Poll();
      if (result == PollResult::kExited) {
        return true;
      }
      if (result == PollResult::kOwnershipError) {
        return false;
      }
      std::this_thread::sleep_for(10ms);
    }
    return Poll() == PollResult::kExited;
  }

 private:
  void ReapRecordedChild() {
    if (recorded_child_pid_ <= 0) {
      return;
    }

    int child_status = 0;
    pid_t result = -1;
    do {
      result = waitpid(recorded_child_pid_, &child_status, WNOHANG);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
      (void)kill(recorded_child_pid_, SIGKILL);
      do {
        result = waitpid(recorded_child_pid_, &child_status, 0);
      } while (result < 0 && errno == EINTR);
    }
  }

  void ReapAdoptedProcessGroup() {
    int child_status = 0;
    while (true) {
      const pid_t result = waitpid(-pid_, &child_status, WNOHANG);
      if (result > 0) {
        continue;
      }
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result < 0 && errno == ECHILD) {
        return;
      }
      if (result == 0) {
        (void)kill(-pid_, SIGKILL);
        while (true) {
          const pid_t reap_result = waitpid(-pid_, &child_status, 0);
          if (reap_result > 0) {
            continue;
          }
          if (reap_result < 0 && errno == EINTR) {
            continue;
          }
          return;
        }
      }
      return;
    }
  }

  void Cleanup() {
    if (cleaned_ || pid_ <= 0) {
      return;
    }
    cleaned_ = true;

    // An external SIGKILL of this harness cannot run destructors. Internal
    // deadlines keep the period requiring scoped cleanup bounded.
    if (!reaped_ && !ownership_error_) {
      (void)kill(-pid_, SIGKILL);
      pid_t result = -1;
      do {
        result = waitpid(pid_, &wait_status_, 0);
      } while (result < 0 && errno == EINTR);
      if (result == pid_) {
        reaped_ = true;
      } else if (result < 0 && errno == ECHILD) {
        ownership_error_ = true;
      }
    }

    ReapRecordedChild();
    ReapAdoptedProcessGroup();
  }

  pid_t pid_;
  pid_t recorded_child_pid_ = -1;
  int wait_status_ = 0;
  bool reaped_ = false;
  bool ownership_error_ = false;
  bool cleaned_ = false;
};

bool WaitForChild(SupervisorProcess* supervisor, pid_t* child_pid) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto poll_result = supervisor->Poll();
    if (poll_result != SupervisorProcess::PollResult::kRunning) {
      return false;
    }
    *child_pid = ReadChildPid(supervisor->pid());
    if (*child_pid > 0) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool TestAbnormalSupervisorCleanup() {
  int child_pipe[2];
  if (pipe(child_pipe) != 0) {
    std::cerr << "failed to create abnormal-cleanup pipe\n";
    return false;
  }

  const pid_t supervisor_pid = fork();
  if (supervisor_pid < 0) {
    close(child_pipe[0]);
    close(child_pipe[1]);
    std::cerr << "failed to fork abnormal supervisor\n";
    return false;
  }
  if (supervisor_pid == 0) {
    close(child_pipe[0]);
    (void)setpgid(0, 0);
    const pid_t child_pid = fork();
    if (child_pid < 0) {
      _exit(126);
    }
    if (child_pid == 0) {
      close(child_pipe[1]);
      while (true) {
        pause();
      }
    }
    const ssize_t written =
        write(child_pipe[1], &child_pid, sizeof(child_pid));
    close(child_pipe[1]);
    _exit(written == static_cast<ssize_t>(sizeof(child_pid)) ? 23 : 125);
  }

  close(child_pipe[1]);
  (void)setpgid(supervisor_pid, supervisor_pid);
  pid_t child_pid = -1;
  {
    SupervisorProcess supervisor(supervisor_pid);
    const ssize_t bytes_read =
        read(child_pipe[0], &child_pid, sizeof(child_pid));
    close(child_pipe[0]);
    if (bytes_read != static_cast<ssize_t>(sizeof(child_pid)) ||
        child_pid <= 0) {
      std::cerr << "failed to receive abnormal child PID\n";
      return false;
    }
    supervisor.RecordChild(child_pid);
    if (!supervisor.WaitForExit(2s) ||
        !WIFEXITED(supervisor.wait_status()) ||
        WEXITSTATUS(supervisor.wait_status()) != 23) {
      std::cerr << "abnormal supervisor did not exit as expected\n";
      return false;
    }
  }

  int child_status = 0;
  const pid_t child_result = waitpid(child_pid, &child_status, WNOHANG);
  if (child_result < 0 && errno == ECHILD) {
    return true;
  }
  if (child_result == 0) {
    (void)kill(-supervisor_pid, SIGKILL);
    while (waitpid(child_pid, &child_status, 0) < 0 && errno == EINTR) {
    }
  }
  std::cerr << "adopted child was not cleaned up and reaped\n";
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: supervisor_signal_test <supervisor> <daemon>\n";
    return 1;
  }
  if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
    std::cerr << "failed to enable child subreaper\n";
    return 1;
  }
  if (!TestAbnormalSupervisorCleanup()) {
    return 1;
  }

  DefaultPortCollision port_collision;
  if (!port_collision.valid()) {
    std::cerr << "failed to establish default-port collision\n";
    return 1;
  }

  char database_template[] = "/tmp/opcua-supervisor-signal-XXXXXX";
  const int database_fd = mkstemp(database_template);
  if (database_fd < 0) {
    std::cerr << "failed to create temporary database path\n";
    return 1;
  }
  close(database_fd);
  DatabaseCleanup database(database_template);

  const pid_t supervisor_pid = fork();
  if (supervisor_pid < 0) {
    std::cerr << "failed to fork supervisor\n";
    return 1;
  }
  if (supervisor_pid == 0) {
    (void)setpgid(0, 0);
    execl(argv[1], argv[1], database.path().c_str(), argv[2], "0", nullptr);
    _exit(127);
  }
  (void)setpgid(supervisor_pid, supervisor_pid);
  SupervisorProcess supervisor(supervisor_pid);

  pid_t daemon_pid = -1;
  if (!WaitForChild(&supervisor, &daemon_pid)) {
    std::cerr << "supervisor daemon child did not start\n";
    return 1;
  }
  supervisor.RecordChild(daemon_pid);

  const auto shutdown_started = std::chrono::steady_clock::now();
  if (kill(supervisor.pid(), SIGTERM) != 0 || !supervisor.WaitForExit(10s)) {
    std::cerr << "supervisor did not stop after SIGTERM\n";
    return 1;
  }
  const auto shutdown_duration = std::chrono::steady_clock::now() -
                                 shutdown_started;
  if (shutdown_duration >= 4s) {
    std::cerr << "supervisor graceful shutdown exceeded threshold: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     shutdown_duration)
                     .count()
              << "ms\n";
    return 1;
  }
  std::cout << "graceful shutdown elapsed: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                   shutdown_duration)
                   .count()
            << "ms\n";

  const int wait_status = supervisor.wait_status();
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    std::cerr << "signal-triggered supervisor exit was not clean\n";
    return 1;
  }
  if (kill(daemon_pid, 0) == 0 || errno != ESRCH) {
    std::cerr << "daemon remained after supervisor exit\n";
    return 1;
  }
  return 0;
}
