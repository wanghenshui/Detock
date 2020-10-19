#include "paxos/simple_multi_paxos.h"

#include "connection/broker.h"
#include "connection/sender.h"

namespace slog {

using internal::Request;
using internal::Response;

SimpleMultiPaxos::SimpleMultiPaxos(
    Channel group_number,
    const shared_ptr<Broker>& broker,
    const vector<MachineId>& members,
    MachineId me)
  : NetworkedModule(broker, group_number),
    leader_(*this, members, me),
    acceptor_(*this) {}

void SimpleMultiPaxos::HandleInternalRequest(ReusableRequest&& req, MachineId from) {
  leader_.HandleRequest(*req.get());
  acceptor_.HandleRequest(*req.get(), from);
}

void SimpleMultiPaxos::HandleInternalResponse(ReusableResponse&& res, MachineId from) {
  leader_.HandleResponse(*res.get(), from);
}

bool SimpleMultiPaxos::IsMember() const {
  return leader_.IsMember();
}

void SimpleMultiPaxos::SendSameChannel(
    const google::protobuf::Message& msg,
    MachineId to_machine_id) {
  Send(msg, channel(), to_machine_id);
}

} // namespace slog