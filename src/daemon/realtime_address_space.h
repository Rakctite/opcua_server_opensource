#ifndef OPCUA_SERVER_SRC_DAEMON_REALTIME_ADDRESS_SPACE_H_
#define OPCUA_SERVER_SRC_DAEMON_REALTIME_ADDRESS_SPACE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "daemon/realtime_value_store.h"

namespace opcua {

struct RealtimeNodeConfig {
  std::uint32_t node_id;
  std::string browse_name;
  ScalarType type;
  ValueSlotId slot_id;
};

class RealtimeAddressSpace {
 public:
  // The store and every non-detached server passed to AddNode must outlive this
  // object.
  explicit RealtimeAddressSpace(RealtimeValueStore* store);
  ~RealtimeAddressSpace() noexcept;
  void DetachServer(UA_Server* server) noexcept;
  Status AddNode(UA_Server* server, const RealtimeNodeConfig& config);

 private:
  struct NodeContext;
  static UA_StatusCode Read(UA_Server* server, const UA_NodeId* session_id,
                            void* session_context, const UA_NodeId* node_id,
                            void* node_context,
                            UA_Boolean include_source_timestamp,
                            const UA_NumericRange* range, UA_DataValue* value);

  RealtimeValueStore* store_;
  std::vector<std::unique_ptr<NodeContext>> contexts_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_DAEMON_REALTIME_ADDRESS_SPACE_H_
