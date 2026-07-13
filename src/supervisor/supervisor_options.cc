#include "supervisor/supervisor_options.h"

#include <charconv>
#include <system_error>

namespace opcua {

Result<int> ParseApiPort(const std::string& value) {
  int port = 0;
  const auto parse_result =
      std::from_chars(value.data(), value.data() + value.size(), port);
  if (value.empty() || parse_result.ec != std::errc() ||
      parse_result.ptr != value.data() + value.size() || port < 0 ||
      port > 65535) {
    return Status::Error("API port must be an integer between 0 and 65535");
  }
  return port;
}

}  // namespace opcua
