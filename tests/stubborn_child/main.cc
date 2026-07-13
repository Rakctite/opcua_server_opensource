#include <chrono>
#include <csignal>
#include <fstream>
#include <thread>

namespace {

volatile std::sig_atomic_t termination_requested = 0;

void HandleTermination(int) { termination_requested = 1; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return 1;
  }
  std::signal(SIGTERM, HandleTermination);
  {
    std::ofstream marker(argv[1]);
    marker << "ready\n";
  }
  while (true) {
    if (termination_requested != 0) {
      std::ofstream marker(argv[1]);
      marker << "term\n";
      termination_requested = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}
