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
    ~Peer();

    int send_rpc(uint16_t service_id, uint16_t opcode, const std::string& req) const;

    std::string str() const {
        std::stringstream ss;

        ss  << "peer info: " << std::endl
            << "    id: " << id_ << std::endl
            << "    addr&port: " << addr_ << " " << port_ << std::endl
            << "    next_index: " << next_index_ << std::endl
            << "    match_index: " << match_index_ << std::endl;

        return ss.str();
    }

public:

    const uint64_t id_;

    const std::string addr_;
    uint16_t port_;
    rpc_handler_t handler_;


    std::unique_ptr<RpcClient> rpc_client_;

    // 对应于需要发送的下一条日志的索引值，初始化为leader的日志最后索引值+1
    uint64_t next_index_;

    // 已经复制给peer的最高索引值
    uint64_t match_index_;

    friend std::ostream& operator<<(std::ostream& os, const Peer& peer);

};

} // namespace sisyphus

#endif // __RAFT_PEER_H__
