#ifndef OPCUA_SERVER_SRC_COMMON_RESULT_H_
#define OPCUA_SERVER_SRC_COMMON_RESULT_H_

#include <string>
#include <utility>
#include <variant>

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
  Result(T value) : storage_(std::move(value)) {}
  Result(Status status) : storage_(NormalizeErrorStatus(std::move(status))) {}

  static Result Error(Status status) { return Result(std::move(status)); }

  bool ok() const { return std::holds_alternative<T>(storage_); }
  const T& value() const { return std::get<T>(storage_); }
  T& value() { return std::get<T>(storage_); }
  const Status& status() const {
    if (ok()) {
      return OkStatus();
    }
    return std::get<Status>(storage_);
  }

 private:
  static Status NormalizeErrorStatus(Status status) {
    if (status.ok()) {
      return Status::Error("Result error constructed with OK status");
    }
    return status;
  }

  static const Status& OkStatus() {
    static const Status ok_status = Status::Ok();
    return ok_status;
  }

  std::variant<T, Status> storage_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_COMMON_RESULT_H_
