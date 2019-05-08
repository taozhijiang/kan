#ifndef __RAFT_PEER_H__
#define __RAFT_PEER_H__

#include <xtra_rhel.h>

#include <Client/include/RpcClient.h>


namespace sisyphus {

using tzrpc_client::rpc_handler_t;
using tzrpc_client::RpcClient;


class Peer {

public:
    Peer(uint64_t id, 
         const std::string& addr, uint16_t port, const rpc_handler_t& handler);
    ~Peer() ;

    int send_rpc(uint16_t service_id, uint16_t opcode, const std::string& req);

public:

    const uint64_t id_;

    const std::string addr_;
    uint16_t port_;
    rpc_handler_t handler_;


    std::unique_ptr<RpcClient> rpc_client_;


    uint64_t next_index_;
    uint64_t match_index_;
};

} // namespace sisyphus

#endif // __RAFT_PEER_H__
