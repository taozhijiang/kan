#include <Raft/Peer.h>


namespace sisyphus {

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


int Peer::send_rpc(uint16_t service_id, uint16_t opcode, const std::string& req) {

    return 0;
}


} // namespace sisyphus

