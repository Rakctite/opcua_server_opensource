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
  explicit SupervisorProcess(pid_t pid) : pid_(pid) {}
  ~SupervisorProcess() { Cleanup(); }

  pid_t pid() const { return pid_; }
  int wait_status() const { return wait_status_; }

  bool PollExited() {
    if (reaped_) {
      return true;
    }
    const pid_t result = waitpid(pid_, &wait_status_, WNOHANG);
    if (result == pid_) {
      reaped_ = true;
      return true;
    }
    if (result < 0 && errno == ECHILD) {
      reaped_ = true;
      return true;
    }
    return false;
  }

  bool WaitForExit(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (PollExited()) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return PollExited();
  }

 private:
  void Cleanup() {
    if (reaped_ || pid_ <= 0) {
      return;
    }

    const pid_t current_child = ReadChildPid(pid_);
    if (current_child > 0) {
      (void)kill(current_child, SIGKILL);
    }
    (void)kill(pid_, SIGKILL);
    while (waitpid(pid_, &wait_status_, 0) < 0 && errno == EINTR) {
    }
    reaped_ = true;
  }

  pid_t pid_;
  int wait_status_ = 0;
  bool reaped_ = false;
};

bool WaitForChild(SupervisorProcess* supervisor, pid_t* child_pid) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (supervisor->PollExited()) {
      return false;
    }
    *child_pid = ReadChildPid(supervisor->pid());
    if (*child_pid > 0) {
      return true;
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
    execl(argv[1], argv[1], database.path().c_str(), argv[2], "0", nullptr);
    _exit(127);
  }
  SupervisorProcess supervisor(supervisor_pid);

  pid_t daemon_pid = -1;
  if (!WaitForChild(&supervisor, &daemon_pid)) {
    std::cerr << "supervisor daemon child did not start\n";
    return 1;
  }

  if (kill(supervisor.pid(), SIGTERM) != 0 || !supervisor.WaitForExit(10s)) {
    std::cerr << "supervisor did not stop after SIGTERM\n";
    return 1;
  }

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
