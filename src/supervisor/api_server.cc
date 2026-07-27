#include "supervisor/api_server.h"

#include <httplib.h>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>

#include "config/config_repository.h"
#include "supervisor/config_json_codec.h"
#include "supervisor/config_json_codec_internal.h"
#include "supervisor/process_controller.h"

namespace opcua {
namespace {

constexpr std::size_t kMaxConfigJsonBytes = 64U * 1024U;
constexpr auto kDaemonStopTimeout = std::chrono::milliseconds(5000);
constexpr const char* kJsonContentType = "application/json";

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
  SetJson(response, status, JsonError(message));
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
    if (port < 0 || port > 65535) {
      return Status::Error("API server port must be between 0 and 65535");
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
                ",\"diagnostic\":\"" +
                internal::EscapeJsonString(status.diagnostic) +
                "\"}");
  }

  void HandleGetConfig(httplib::Response* response) {
    std::lock_guard<std::mutex> lock(operations_mutex_);
    auto config_result = repository_->Load();
    if (!config_result.ok()) {
      SetError(response, 500, config_result.status().message());
      return;
    }
    SetJson(response, 200, ServerConfigToJson(config_result.value()));
  }

  void HandlePutConfig(const httplib::Request& request,
                       httplib::Response* response) {
    auto config_result = ParseServerConfigJson(request.body);
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
