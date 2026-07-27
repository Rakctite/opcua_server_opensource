#include "config/mqtt_config.h"

#include <cctype>
#include <string>
#include <string_view>

namespace opcua {
namespace {

bool IsValidIpv4Address(std::string_view address) {
  if (address.empty() || address.back() == '.') {
    return false;
  }
  int octet_count = 0;
  std::size_t start = 0;
  while (start < address.size()) {
    const std::size_t separator = address.find('.', start);
    const std::size_t end =
        separator == std::string_view::npos ? address.size() : separator;
    const std::string_view octet = address.substr(start, end - start);
    if (octet.empty() || octet.size() > 3 ||
        (octet.size() > 1 && octet.front() == '0')) {
      return false;
    }

    int value = 0;
    for (const unsigned char character : octet) {
      if (std::isdigit(character) == 0) {
        return false;
      }
      value = value * 10 + static_cast<int>(character - '0');
    }
    if (value > 255) {
      return false;
    }

    ++octet_count;
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return octet_count == 4;
}

bool CountIpv6Groups(std::string_view groups, bool ipv4_tail_allowed,
                     int* group_count) {
  if (groups.empty()) {
    return true;
  }
  if (groups.back() == ':') {
    return false;
  }

  std::size_t start = 0;
  while (start < groups.size()) {
    const std::size_t separator = groups.find(':', start);
    const std::size_t end =
        separator == std::string_view::npos ? groups.size() : separator;
    const std::string_view group = groups.substr(start, end - start);
    if (group.empty()) {
      return false;
    }

    if (group.find('.') != std::string_view::npos) {
      if (!ipv4_tail_allowed || separator != std::string_view::npos ||
          !IsValidIpv4Address(group)) {
        return false;
      }
      *group_count += 2;
    } else {
      if (group.size() > 4) {
        return false;
      }
      for (const unsigned char character : group) {
        if (std::isxdigit(character) == 0) {
          return false;
        }
      }
      ++*group_count;
    }

    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return true;
}

bool IsValidIpv6Address(std::string_view address) {
  const std::size_t compression = address.find("::");
  int group_count = 0;
  if (compression == std::string_view::npos) {
    return CountIpv6Groups(address, true, &group_count) && group_count == 8;
  }
  if (address.find("::", compression + 2) != std::string_view::npos) {
    return false;
  }

  const std::string_view leading = address.substr(0, compression);
  const std::string_view trailing = address.substr(compression + 2);
  return CountIpv6Groups(leading, false, &group_count) &&
         CountIpv6Groups(trailing, true, &group_count) && group_count < 8;
}

bool IsDottedNumericHost(std::string_view host) {
  bool has_dot = false;
  for (const unsigned char character : host) {
    if (character == '.') {
      has_dot = true;
    } else if (std::isdigit(character) == 0) {
      return false;
    }
  }
  return has_dot;
}

bool IsValidDnsHost(std::string_view host) {
  if (host.empty() || host.size() > 253) {
    return false;
  }

  std::size_t label_length = 0;
  bool previous_was_hyphen = false;
  for (const unsigned char character : host) {
    if (character == '.') {
      if (label_length == 0 || previous_was_hyphen) {
        return false;
      }
      label_length = 0;
      previous_was_hyphen = false;
      continue;
    }
    if (std::isalnum(character) == 0 && character != '-') {
      return false;
    }
    if (label_length == 0 && character == '-') {
      return false;
    }
    ++label_length;
    if (label_length > 63) {
      return false;
    }
    previous_was_hyphen = character == '-';
  }
  return label_length > 0 && !previous_was_hyphen;
}

bool IsValidTcpBrokerUri(const std::string& broker_uri) {
  constexpr char kTcpScheme[] = "tcp://";
  constexpr std::size_t kTcpSchemeLength = sizeof(kTcpScheme) - 1;
  if (broker_uri.compare(0, kTcpSchemeLength, kTcpScheme) != 0) {
    return false;
  }
  for (const unsigned char character : broker_uri) {
    if (character == '\0' || std::isspace(character) != 0) {
      return false;
    }
  }

  const std::string authority = broker_uri.substr(kTcpSchemeLength);
  if (authority.empty() || authority.find_first_of("/?#") != std::string::npos) {
    return false;
  }

  std::size_t port_separator = std::string::npos;
  if (authority.front() == '[') {
    const std::size_t closing_bracket = authority.find(']');
    if (closing_bracket == std::string::npos || closing_bracket == 1 ||
        closing_bracket + 1 >= authority.size() ||
        authority[closing_bracket + 1] != ':' ||
        authority.find('[', 1) != std::string::npos ||
        authority.find(']', closing_bracket + 1) != std::string::npos) {
      return false;
    }
    if (!IsValidIpv6Address(
            std::string_view(authority).substr(1, closing_bracket - 1))) {
      return false;
    }
    port_separator = closing_bracket + 1;
  } else {
    port_separator = authority.rfind(':');
    if (port_separator == std::string::npos || port_separator == 0 ||
        authority.find(':') != port_separator ||
        authority.find_first_of("[]") != std::string::npos) {
      return false;
    }
    const std::string_view host =
        std::string_view(authority).substr(0, port_separator);
    if (IsDottedNumericHost(host) ? !IsValidIpv4Address(host)
                                  : !IsValidDnsHost(host)) {
      return false;
    }
  }

  const std::string port_text = authority.substr(port_separator + 1);
  if (port_text.empty()) {
    return false;
  }
  int port = 0;
  for (const unsigned char character : port_text) {
    if (std::isdigit(character) == 0) {
      return false;
    }
    port = port * 10 + static_cast<int>(character - '0');
    if (port > 65535) {
      return false;
    }
  }
  return port > 0;
}

}  // namespace

MqttConfig MqttConfig::Default() {
  return MqttConfig{false,
                    "tcp://127.0.0.1:1883",
                    "opcua-server",
                    "test/temperature",
                    1,
                    1001U,
                    "Temperature",
                    "double",
                    5000};
}

Status MqttConfig::Validate() const {
  if (!IsValidTcpBrokerUri(broker_uri)) {
    return Status::Error("mqtt.broker_uri must be a valid tcp://host:port URI");
  }
  if (client_id.empty() || client_id.find('\0') != std::string::npos) {
    return Status::Error("mqtt.client_id must be nonempty and contain no NUL");
  }
  if (topic.empty() || topic.find('\0') != std::string::npos ||
      topic.find_first_of("+#") != std::string::npos) {
    return Status::Error(
        "mqtt.topic must be nonempty and contain no NUL or wildcards");
  }
  if (qos != 1) {
    return Status::Error("only mqtt.qos=1 is supported in the MVP");
  }
  if (node_id == 0) {
    return Status::Error("mqtt.node_id must be positive");
  }
  if (browse_name.empty()) {
    return Status::Error("mqtt.browse_name must not be empty");
  }
  if (data_type != "boolean" && data_type != "int64" &&
      data_type != "double") {
    return Status::Error("mqtt.data_type must be boolean, int64, or double");
  }
  if (stale_timeout_ms <= 0) {
    return Status::Error("mqtt.stale_timeout_ms must be positive");
  }
  return Status::Ok();
}

}  // namespace opcua
