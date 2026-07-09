#include <chrono>
#include <csignal>
#include <thread>

namespace {
volatile std::sig_atomic_t running = 1;
void Stop(int) { running = 0; }
}  // namespace

int main() {
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  while (running != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return 0;
}
