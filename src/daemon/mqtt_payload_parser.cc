#include "daemon/mqtt_payload_parser.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>

namespace opcua {
namespace {

Result<ScalarValue> InvalidPayload() {
  return Status::Error("invalid MQTT scalar payload");
}

}  // namespace

Result<ScalarValue> ParseMqttScalar(std::string_view payload, ScalarType type) {
  if (payload.empty() || payload.size() > kMaxMqttScalarPayloadBytes ||
      payload.find('\0') != std::string_view::npos) {
    return InvalidPayload();
  }

  const char* const end = payload.data() + payload.size();
  switch (type) {
    case ScalarType::kBoolean:
      if (payload == "true") {
        return ScalarValue(true);
      }
      if (payload == "false") {
        return ScalarValue(false);
      }
      return InvalidPayload();

    case ScalarType::kInt64: {
      std::int64_t value = 0;
      const auto result = std::from_chars(payload.data(), end, value);
      if (result.ec != std::errc() || result.ptr != end) {
        return InvalidPayload();
      }
      return ScalarValue(value);
    }

    case ScalarType::kDouble: {
      double value = 0.0;
      const auto result = std::from_chars(payload.data(), end, value);
      if (result.ec != std::errc() || result.ptr != end ||
          !std::isfinite(value)) {
        return InvalidPayload();
      }
      return ScalarValue(value);
    }
  }

  return InvalidPayload();
}

}  // namespace opcua
