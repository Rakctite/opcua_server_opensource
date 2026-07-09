#include <iostream>
#include <memory>

#include "common/result.h"

namespace {

class MoveOnlyNoDefault {
 public:
  explicit MoveOnlyNoDefault(int value) : value_(value) {}
  MoveOnlyNoDefault(const MoveOnlyNoDefault&) = delete;
  MoveOnlyNoDefault& operator=(const MoveOnlyNoDefault&) = delete;
  MoveOnlyNoDefault(MoveOnlyNoDefault&&) = default;
  MoveOnlyNoDefault& operator=(MoveOnlyNoDefault&&) = default;

 private:
  int value_;
};

opcua::Result<MoveOnlyNoDefault> MakeErrorResult() {
  return opcua::Status::Error("scaffold error");
}

opcua::Result<std::unique_ptr<int>> MakeValueResult() {
  return std::make_unique<int>(1);
}

}  // namespace

int main() {
  const auto error_result = MakeErrorResult();
  auto value_result = MakeValueResult();
  if (error_result.ok() || !value_result.ok()) {
    return 1;
  }

  std::cout << "opcua-daemon scaffold\n";
  return 0;
}
