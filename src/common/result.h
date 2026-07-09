#ifndef OPCUA_SERVER_SRC_COMMON_RESULT_H_
#define OPCUA_SERVER_SRC_COMMON_RESULT_H_

#include <string>
#include <utility>

namespace opcua {

class Status {
 public:
  static Status Ok() { return Status(true, ""); }
  static Status Error(std::string message) {
    return Status(false, std::move(message));
  }

  bool ok() const { return ok_; }
  const std::string& message() const { return message_; }

 private:
  Status(bool ok, std::string message)
      : ok_(ok), message_(std::move(message)) {}

  bool ok_;
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result(T value) : ok_(true), value_(std::move(value)) {}
  Result(Status status) : ok_(false), status_(std::move(status)) {}

  bool ok() const { return ok_; }
  const T& value() const { return value_; }
  T& value() { return value_; }
  const Status& status() const { return status_; }

 private:
  bool ok_;
  T value_{};
  Status status_ = Status::Ok();
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_COMMON_RESULT_H_
