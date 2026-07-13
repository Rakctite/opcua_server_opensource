#include "supervisor/supervisor_options.h"

#include <iostream>
#include <string>

namespace {

bool ExpectPort(const std::string& text, int expected) {
  const auto result = opcua::ParseApiPort(text);
  if (!result.ok() || result.value() != expected) {
    std::cerr << "expected valid API port: " << text << "\n";
    return false;
  }
  return true;
}

bool ExpectInvalid(const std::string& text) {
  if (opcua::ParseApiPort(text).ok()) {
    std::cerr << "expected invalid API port: " << text << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!ExpectPort("0", 0) || !ExpectPort("8080", 8080) ||
      !ExpectPort("65535", 65535) || !ExpectInvalid("") ||
      !ExpectInvalid("-1") || !ExpectInvalid("+80") ||
      !ExpectInvalid(" 80") || !ExpectInvalid("8080x") ||
      !ExpectInvalid("65536")) {
    return 1;
  }
  return 0;
}
