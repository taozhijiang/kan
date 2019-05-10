#include <Raft/Peer.h>


namespace sisyphus {

using tzrpc_client::RpcClientStatus;

Peer::Peer(uint64_t id,
           const std::string& addr, uint16_t port, const rpc_handler_t& handler) :
    id_(id),
    addr_(addr),
    port_(port),
    handler_(handler),
    rpc_client_(),
    next_index_(0),
    match_index_(0) {

    rpc_client_.reset(new RpcClient(addr_, port_, handler_));
}

Peer::~Peer() {
}


int Peer::send_rpc(uint16_t service_id, uint16_t opcode, const std::string& req) const {
    RpcClientStatus status = rpc_client_->call_RPC(service_id, opcode, req);
    return status == RpcClientStatus::OK ? 0 : -1;
}


std::ostream& operator<<(std::ostream& os, const Peer& peer) {
    os << peer.str() << std::endl;
    return os;
}


} // namespace sisyphus

