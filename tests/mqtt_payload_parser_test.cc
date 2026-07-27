#include "daemon/mqtt_payload_parser.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace {

int Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

int TestAcceptedValues() {
  const auto true_result =
      opcua::ParseMqttScalar("true", opcua::ScalarType::kBoolean);
  if (int rc = Expect(true_result.ok() &&
                          std::holds_alternative<bool>(true_result.value()) &&
                          std::get<bool>(true_result.value()),
                      "true should parse as Boolean")) {
    return rc;
  }

  const auto false_result =
      opcua::ParseMqttScalar("false", opcua::ScalarType::kBoolean);
  if (int rc = Expect(false_result.ok() &&
                          std::holds_alternative<bool>(false_result.value()) &&
                          !std::get<bool>(false_result.value()),
                      "false should parse as Boolean")) {
    return rc;
  }

  const auto int64_result = opcua::ParseMqttScalar(
      "-9223372036854775808", opcua::ScalarType::kInt64);
  if (int rc = Expect(
          int64_result.ok() &&
              std::holds_alternative<std::int64_t>(int64_result.value()) &&
              std::get<std::int64_t>(int64_result.value()) ==
                  std::numeric_limits<std::int64_t>::lowest(),
          "INT64_MIN should parse as Int64")) {
    return rc;
  }

  const auto double_result =
      opcua::ParseMqttScalar("37.5", opcua::ScalarType::kDouble);
  return Expect(double_result.ok() &&
                    std::holds_alternative<double>(double_result.value()) &&
                    std::get<double>(double_result.value()) == 37.5,
                "37.5 should parse as Double");
}

int TestPayloadLengthBoundary() {
  const std::string maximum_payload = "0." + std::string(126, '0');
  if (int rc = Expect(
          maximum_payload.size() == opcua::kMaxMqttScalarPayloadBytes,
          "maximum payload fixture should be 128 bytes")) {
    return rc;
  }
  const auto maximum_result = opcua::ParseMqttScalar(
      maximum_payload, opcua::ScalarType::kDouble);
  if (int rc = Expect(maximum_result.ok() &&
                          std::get<double>(maximum_result.value()) == 0.0,
                      "128-byte finite Double should be accepted")) {
    return rc;
  }

  const std::string oversized_payload(
      opcua::kMaxMqttScalarPayloadBytes + 1, '1');
  return Expect(!opcua::ParseMqttScalar(oversized_payload,
                                        opcua::ScalarType::kInt64)
                     .ok(),
                "129-byte payload should be rejected");
}

int TestInvalidBooleanValues() {
  if (int rc = Expect(
          !opcua::ParseMqttScalar("TRUE", opcua::ScalarType::kBoolean).ok(),
          "uppercase Boolean should be rejected")) {
    return rc;
  }
  if (int rc = Expect(
          !opcua::ParseMqttScalar(" true", opcua::ScalarType::kBoolean).ok(),
          "leading Boolean whitespace should be rejected")) {
    return rc;
  }
  return Expect(
      !opcua::ParseMqttScalar("false ", opcua::ScalarType::kBoolean).ok(),
      "trailing Boolean whitespace should be rejected");
}

int TestInvalidInt64Values() {
  if (int rc = Expect(
          !opcua::ParseMqttScalar("", opcua::ScalarType::kInt64).ok(),
          "empty Int64 should be rejected")) {
    return rc;
  }
  if (int rc = Expect(
          !opcua::ParseMqttScalar(" 42", opcua::ScalarType::kInt64).ok(),
          "leading Int64 whitespace should be rejected")) {
    return rc;
  }
  if (int rc = Expect(
          !opcua::ParseMqttScalar("42 ", opcua::ScalarType::kInt64).ok(),
          "trailing Int64 whitespace should be rejected")) {
    return rc;
  }
  if (int rc = Expect(!opcua::ParseMqttScalar("42x",
                                               opcua::ScalarType::kInt64)
                           .ok(),
                      "Int64 suffix should be rejected")) {
    return rc;
  }
  return Expect(!opcua::ParseMqttScalar("9223372036854775808",
                                         opcua::ScalarType::kInt64)
                     .ok(),
                "out-of-range Int64 should be rejected");
}

int TestInvalidDoubleValues() {
  constexpr std::string_view invalid_values[] = {
      "1.0x", "nan", "inf", "1e309", " 1.0", "1.0 ",
  };
  for (const auto value : invalid_values) {
    if (int rc = Expect(
            !opcua::ParseMqttScalar(value, opcua::ScalarType::kDouble).ok(),
            "invalid Double should be rejected")) {
      return rc;
    }
  }
  return 0;
}

int TestEmbeddedNulIsRejected() {
  const std::string payload("12\0", 3);
  return Expect(
      !opcua::ParseMqttScalar(payload, opcua::ScalarType::kInt64).ok(),
      "embedded NUL should be rejected");
}

}  // namespace

int main() {
  if (int rc = TestAcceptedValues()) return rc;
  if (int rc = TestPayloadLengthBoundary()) return rc;
  if (int rc = TestInvalidBooleanValues()) return rc;
  if (int rc = TestInvalidInt64Values()) return rc;
  if (int rc = TestInvalidDoubleValues()) return rc;
  if (int rc = TestEmbeddedNulIsRejected()) return rc;
  return 0;
}
