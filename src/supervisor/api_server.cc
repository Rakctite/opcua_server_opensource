#include "supervisor/api_server.h"

#include <httplib.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "config/config_repository.h"
#include "config/server_config.h"
#include "supervisor/process_controller.h"

namespace opcua {
namespace {

constexpr std::size_t kMaxConfigJsonBytes = 64U * 1024U;
constexpr std::size_t kConfigFieldCount = 13;
constexpr auto kDaemonStopTimeout = std::chrono::milliseconds(5000);
constexpr const char* kJsonContentType = "application/json";

enum class JsonValueKind { kString, kInteger };

struct JsonValue {
  JsonValueKind kind = JsonValueKind::kString;
  std::string string_value;
  int integer_value = 0;
};

using JsonObject = std::unordered_map<std::string, JsonValue>;

bool IsJsonWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

int HexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

void AppendCodePoint(std::uint32_t code_point, std::string* output) {
  if (code_point <= 0x7FU) {
    output->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    output->push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
    output->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    output->push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
    output->push_back(
        static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    output->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    output->push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
    output->push_back(
        static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
    output->push_back(
        static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    output->push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

std::size_t ValidUtf8SequenceLength(const std::string& input,
                                    std::size_t position) {
  const auto first = static_cast<unsigned char>(input[position]);
  std::size_t length = 0;
  std::uint32_t code_point = 0;
  std::uint32_t minimum = 0;
  if (first >= 0xC2U && first <= 0xDFU) {
    length = 2;
    code_point = first & 0x1FU;
    minimum = 0x80U;
  } else if (first >= 0xE0U && first <= 0xEFU) {
    length = 3;
    code_point = first & 0x0FU;
    minimum = 0x800U;
  } else if (first >= 0xF0U && first <= 0xF4U) {
    length = 4;
    code_point = first & 0x07U;
    minimum = 0x10000U;
  } else {
    return 0;
  }
  if (position + length > input.size()) return 0;
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto next = static_cast<unsigned char>(input[position + offset]);
    if ((next & 0xC0U) != 0x80U) return 0;
    code_point = (code_point << 6U) | (next & 0x3FU);
  }
  if (code_point < minimum || code_point > 0x10FFFFU ||
      (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
    return 0;
  }
  return length;
}

std::string EscapeJsonString(const std::string& value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string escaped;
  escaped.reserve(value.size() + 2);
  for (std::size_t position = 0; position < value.size();) {
    const auto ch = static_cast<unsigned char>(value[position]);
    switch (ch) {
      case '"':
        escaped += "\\\"";
        ++position;
        break;
      case '\\':
        escaped += "\\\\";
        ++position;
        break;
      case '\b':
        escaped += "\\b";
        ++position;
        break;
      case '\f':
        escaped += "\\f";
        ++position;
        break;
      case '\n':
        escaped += "\\n";
        ++position;
        break;
      case '\r':
        escaped += "\\r";
        ++position;
        break;
      case '\t':
        escaped += "\\t";
        ++position;
        break;
      default:
        if (ch < 0x20U) {
          escaped += "\\u00";
          escaped.push_back(kHex[(ch >> 4U) & 0x0FU]);
          escaped.push_back(kHex[ch & 0x0FU]);
          ++position;
        } else if (ch < 0x80U) {
          escaped.push_back(static_cast<char>(ch));
          ++position;
        } else {
          const std::size_t length = ValidUtf8SequenceLength(value, position);
          if (length == 0) {
            escaped += "\\ufffd";
            ++position;
          } else {
            escaped.append(value, position, length);
            position += length;
          }
        }
        break;
    }
  }
  return escaped;
}

bool IsKnownField(const std::string& field) {
  return field == "server_application_name" ||
         field == "server_product_uri" || field == "server_bind_address" ||
         field == "server_port" || field == "server_endpoint_path" ||
         field == "security_mode" || field == "security_policy" ||
         field == "max_sessions" || field == "max_subscriptions" ||
         field == "logging_level" || field == "logging_target" ||
         field == "address_space_mode" || field == "address_space_path";
}

class ConfigJsonParser {
 public:
  explicit ConfigJsonParser(const std::string& input) : input_(input) {}

  Result<ServerConfig> Parse() {
    if (input_.size() > kMaxConfigJsonBytes) {
      return Status::Error("configuration JSON exceeds 65536 bytes");
    }

    auto object_result = ParseObject();
    if (!object_result.ok()) return object_result.status();
    SkipWhitespace();
    if (position_ != input_.size()) {
      return Status::Error("trailing data after configuration object");
    }
    return BuildConfig(object_result.value());
  }

 private:
  void SkipWhitespace() {
    while (position_ < input_.size() && IsJsonWhitespace(input_[position_])) {
      ++position_;
    }
  }

  bool Consume(char expected) {
    SkipWhitespace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  Result<std::uint32_t> ParseHexCodeUnit() {
    if (position_ + 4 > input_.size()) {
      return Status::Error("incomplete Unicode escape");
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      const int digit = HexDigit(input_[position_++]);
      if (digit < 0) return Status::Error("invalid Unicode escape");
      value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    return value;
  }

  Result<std::string> ParseString() {
    SkipWhitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
      return Status::Error("expected JSON string");
    }
    ++position_;
    std::string value;
    while (position_ < input_.size()) {
      const auto ch = static_cast<unsigned char>(input_[position_++]);
      if (ch == '"') return value;
      if (ch < 0x20U) {
        return Status::Error("unescaped control character in JSON string");
      }
      if (ch >= 0x80U) {
        --position_;
        const std::size_t length = ValidUtf8SequenceLength(input_, position_);
        if (length == 0) return Status::Error("invalid UTF-8 in JSON string");
        value.append(input_, position_, length);
        position_ += length;
        continue;
      }
      if (ch != '\\') {
        value.push_back(static_cast<char>(ch));
        continue;
      }
      if (position_ >= input_.size()) {
        return Status::Error("incomplete JSON escape");
      }
      const char escape = input_[position_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          value.push_back(escape);
          break;
        case 'b':
          value.push_back('\b');
          break;
        case 'f':
          value.push_back('\f');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        case 'u': {
          auto code_unit_result = ParseHexCodeUnit();
          if (!code_unit_result.ok()) return code_unit_result.status();
          std::uint32_t code_point = code_unit_result.value();
          if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
            if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') {
              return Status::Error("high surrogate missing low surrogate");
            }
            position_ += 2;
            auto low_result = ParseHexCodeUnit();
            if (!low_result.ok()) return low_result.status();
            const std::uint32_t low = low_result.value();
            if (low < 0xDC00U || low > 0xDFFFU) {
              return Status::Error("invalid low surrogate");
            }
            code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                         (low - 0xDC00U);
          } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
            return Status::Error("unexpected low surrogate");
          }
          if (code_point == 0) {
            return Status::Error("NUL is not allowed in configuration strings");
          }
          AppendCodePoint(code_point, &value);
          break;
        }
        default:
          return Status::Error("invalid JSON escape");
      }
    }
    return Status::Error("unterminated JSON string");
  }

  Result<int> ParseInteger() {
    SkipWhitespace();
    const std::size_t begin = position_;
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return Status::Error("expected JSON integer");
    }
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && input_[position_] >= '0' &&
          input_[position_] <= '9') {
        return Status::Error("JSON integer has a leading zero");
      }
    } else {
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    int value = 0;
    const char* first = input_.data() + begin;
    const char* last = input_.data() + position_;
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc() || result.ptr != last) {
      return Status::Error("JSON integer is out of range");
    }
    return value;
  }

  Result<JsonObject> ParseObject() {
    if (!Consume('{')) return Status::Error("expected JSON object");
    JsonObject object;
    SkipWhitespace();
    if (Consume('}')) return object;

    while (true) {
      auto key_result = ParseString();
      if (!key_result.ok()) return key_result.status();
      const std::string& key = key_result.value();
      if (!IsKnownField(key)) return Status::Error("unknown field: " + key);
      if (!Consume(':')) return Status::Error("expected colon after field name");

      JsonValue value;
      SkipWhitespace();
      if (position_ < input_.size() && input_[position_] == '"') {
        auto string_result = ParseString();
        if (!string_result.ok()) return string_result.status();
        value.kind = JsonValueKind::kString;
        value.string_value = std::move(string_result.value());
      } else {
        auto integer_result = ParseInteger();
        if (!integer_result.ok()) return integer_result.status();
        value.kind = JsonValueKind::kInteger;
        value.integer_value = integer_result.value();
      }
      if (!object.emplace(key, std::move(value)).second) {
        return Status::Error("duplicate field: " + key);
      }

      SkipWhitespace();
      if (Consume('}')) break;
      if (!Consume(',')) return Status::Error("expected comma between fields");
    }
    return object;
  }

  static Status ReadString(const JsonObject& object, const char* field,
                           std::string* output) {
    const auto it = object.find(field);
    if (it == object.end()) return Status::Error("missing field: " + std::string(field));
    if (it->second.kind != JsonValueKind::kString) {
      return Status::Error("field must be a string: " + std::string(field));
    }
    *output = it->second.string_value;
    return Status::Ok();
  }

  static Status ReadInteger(const JsonObject& object, const char* field,
                            int* output) {
    const auto it = object.find(field);
    if (it == object.end()) return Status::Error("missing field: " + std::string(field));
    if (it->second.kind != JsonValueKind::kInteger) {
      return Status::Error("field must be an integer: " + std::string(field));
    }
    *output = it->second.integer_value;
    return Status::Ok();
  }

  static Result<ServerConfig> BuildConfig(const JsonObject& object) {
    if (object.size() != kConfigFieldCount) {
      return Status::Error("configuration object is incomplete");
    }
    ServerConfig config;
    if (auto status = ReadString(object, "server_application_name",
                                 &config.server_application_name);
        !status.ok()) return status;
    if (auto status = ReadString(object, "server_product_uri",
                                 &config.server_product_uri);
        !status.ok()) return status;
    if (auto status = ReadString(object, "server_bind_address",
                                 &config.server_bind_address);
        !status.ok()) return status;
    if (auto status = ReadInteger(object, "server_port", &config.server_port);
        !status.ok()) return status;
    if (auto status = ReadString(object, "server_endpoint_path",
                                 &config.server_endpoint_path);
        !status.ok()) return status;
    if (auto status = ReadString(object, "security_mode", &config.security_mode);
        !status.ok()) return status;
    if (auto status = ReadString(object, "security_policy",
                                 &config.security_policy);
        !status.ok()) return status;
    if (auto status = ReadInteger(object, "max_sessions", &config.max_sessions);
        !status.ok()) return status;
    if (auto status = ReadInteger(object, "max_subscriptions",
                                  &config.max_subscriptions);
        !status.ok()) return status;
    if (auto status = ReadString(object, "logging_level", &config.logging_level);
        !status.ok()) return status;
    if (auto status = ReadString(object, "logging_target",
                                 &config.logging_target);
        !status.ok()) return status;
    if (auto status = ReadString(object, "address_space_mode",
                                 &config.address_space_mode);
        !status.ok()) return status;
    if (auto status = ReadString(object, "address_space_path",
                                 &config.address_space_path);
        !status.ok()) return status;
    return config;
  }

  const std::string& input_;
  std::size_t position_ = 0;
};

std::string ConfigToJson(const ServerConfig& config) {
  return "{\"server_application_name\":\"" +
         EscapeJsonString(config.server_application_name) +
         "\",\"server_product_uri\":\"" +
         EscapeJsonString(config.server_product_uri) +
         "\",\"server_bind_address\":\"" +
         EscapeJsonString(config.server_bind_address) +
         "\",\"server_port\":" + std::to_string(config.server_port) +
         ",\"server_endpoint_path\":\"" +
         EscapeJsonString(config.server_endpoint_path) +
         "\",\"security_mode\":\"" + EscapeJsonString(config.security_mode) +
         "\",\"security_policy\":\"" +
         EscapeJsonString(config.security_policy) +
         "\",\"max_sessions\":" + std::to_string(config.max_sessions) +
         ",\"max_subscriptions\":" +
         std::to_string(config.max_subscriptions) +
         ",\"logging_level\":\"" + EscapeJsonString(config.logging_level) +
         "\",\"logging_target\":\"" + EscapeJsonString(config.logging_target) +
         "\",\"address_space_mode\":\"" +
         EscapeJsonString(config.address_space_mode) +
         "\",\"address_space_path\":\"" +
         EscapeJsonString(config.address_space_path) + "\"}";
}

const char* ProcessStateName(ProcessState state) {
  switch (state) {
    case ProcessState::kStopped:
      return "stopped";
    case ProcessState::kRunning:
      return "running";
    case ProcessState::kCrashed:
      return "crashed";
  }
  return "crashed";
}

void SetJson(httplib::Response* response, int status, std::string body) {
  response->status = status;
  response->set_content(std::move(body), kJsonContentType);
}

void SetError(httplib::Response* response, int status,
              const std::string& message) {
  SetJson(response, status,
          "{\"error\":\"" + EscapeJsonString(message) + "\"}");
}

}  // namespace

class ApiServer::Impl {
 public:
  Impl(ConfigRepository* repository, ProcessController* controller)
      : repository_(repository), controller_(controller) {
    server_.set_payload_max_length(kMaxConfigJsonBytes + 1);
    server_.set_error_handler(
        [](const httplib::Request&, httplib::Response& res) {
          if (res.status == 413) {
            SetError(&res, 400,
                     "configuration request exceeds 65536 bytes");
          }
        });
    server_.set_start_handler([this] {
      bool stop_requested = false;
      {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        lifecycle_state_ = LifecycleState::kRunning;
        stop_requested = stop_requested_;
      }
      if (stop_requested) server_.stop();
    });
    server_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
      SetJson(&res, 200, "{\"status\":\"ok\"}");
    });
    server_.Get("/api/v1/status",
                [this](const httplib::Request&, httplib::Response& res) {
                  HandleStatus(&res);
                });
    server_.Get("/api/v1/config",
                [this](const httplib::Request&, httplib::Response& res) {
                  HandleGetConfig(&res);
                });
    server_.Put("/api/v1/config",
                [this](const httplib::Request& req, httplib::Response& res) {
                  HandlePutConfig(req, &res);
                });
    server_.Post("/api/v1/daemon/start",
                 [this](const httplib::Request&, httplib::Response& res) {
                   HandleStart(&res);
                 });
    server_.Post("/api/v1/daemon/stop",
                 [this](const httplib::Request&, httplib::Response& res) {
                   HandleStop(&res);
                 });
    server_.Post("/api/v1/daemon/restart",
                 [this](const httplib::Request&, httplib::Response& res) {
                   HandleRestart(&res);
                 });
  }

  Status Run(const std::string& bind_address, int port) {
    if (repository_ == nullptr || controller_ == nullptr) {
      return Status::Error("API server dependencies must not be null");
    }
    if (port < 1 || port > 65535) {
      return Status::Error("API server port must be between 1 and 65535");
    }
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (lifecycle_state_ != LifecycleState::kIdle) {
        return Status::Error("API server can only be run once");
      }
      if (stop_requested_) {
        lifecycle_state_ = LifecycleState::kStopped;
        return Status::Ok();
      }
      lifecycle_state_ = LifecycleState::kStarting;
    }
    if (!server_.bind_to_port(bind_address, port)) {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      lifecycle_state_ = LifecycleState::kStopped;
      return Status::Error("failed to bind API server to " + bind_address +
                           ":" + std::to_string(port));
    }
    const bool listen_succeeded = server_.listen_after_bind();
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      lifecycle_state_ = LifecycleState::kStopped;
    }
    if (!listen_succeeded) {
      return Status::Error("API server listener failed");
    }
    return Status::Ok();
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      stop_requested_ = true;
    }
    server_.stop();
  }

 private:
  enum class LifecycleState { kIdle, kStarting, kRunning, kStopped };

  void HandleStatus(httplib::Response* response) {
    std::lock_guard<std::mutex> lock(operations_mutex_);
    const ProcessStatus status = controller_->status();
    SetJson(response, 200,
            "{\"state\":\"" + std::string(ProcessStateName(status.state)) +
                "\",\"exit_code\":" + std::to_string(status.exit_code) +
                ",\"diagnostic\":\"" + EscapeJsonString(status.diagnostic) +
                "\"}");
  }

  void HandleGetConfig(httplib::Response* response) {
    std::lock_guard<std::mutex> lock(operations_mutex_);
    auto config_result = repository_->Load();
    if (!config_result.ok()) {
      SetError(response, 500, config_result.status().message());
      return;
    }
    SetJson(response, 200, ConfigToJson(config_result.value()));
  }

  void HandlePutConfig(const httplib::Request& request,
                       httplib::Response* response) {
    auto config_result = ConfigJsonParser(request.body).Parse();
    if (!config_result.ok()) {
      SetError(response, 400, config_result.status().message());
      return;
    }
    auto validation_status = config_result.value().Validate();
    if (!validation_status.ok()) {
      SetError(response, 400, validation_status.message());
      return;
    }

    std::lock_guard<std::mutex> lock(operations_mutex_);
    auto save_status = repository_->Save(config_result.value());
    if (!save_status.ok()) {
      SetError(response, 500, save_status.message());
      return;
    }
    SetJson(response, 200, "{\"status\":\"ok\"}");
  }

  void HandleStart(httplib::Response* response) {
    std::lock_guard<std::mutex> lock(operations_mutex_);
    SetLifecycleResult(controller_->Start(), response);
  }

  void HandleStop(httplib::Response* response) {
    std::lock_guard<std::mutex> lock(operations_mutex_);
    SetLifecycleResult(controller_->Stop(kDaemonStopTimeout), response);
  }

  void HandleRestart(httplib::Response* response) {
    std::lock_guard<std::mutex> lock(operations_mutex_);
    SetLifecycleResult(controller_->Restart(kDaemonStopTimeout), response);
  }

  static void SetLifecycleResult(const Status& status,
                                 httplib::Response* response) {
    if (!status.ok()) {
      SetError(response, 500, status.message());
      return;
    }
    SetJson(response, 200, "{\"status\":\"ok\"}");
  }

  ConfigRepository* repository_;
  ProcessController* controller_;
  httplib::Server server_;
  std::mutex operations_mutex_;
  std::mutex lifecycle_mutex_;
  LifecycleState lifecycle_state_ = LifecycleState::kIdle;
  bool stop_requested_ = false;
};

ApiServer::ApiServer(ConfigRepository* repository, ProcessController* controller)
    : impl_(std::make_unique<Impl>(repository, controller)) {}

ApiServer::~ApiServer() { Stop(); }

Status ApiServer::Run(const std::string& bind_address, int port) {
  return impl_->Run(bind_address, port);
}

void ApiServer::Stop() { impl_->Stop(); }

}  // namespace opcua
