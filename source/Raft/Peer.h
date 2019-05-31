/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __RAFT_PEER_H__
#define __RAFT_PEER_H__

#include <xtra_rhel.h>

#include <Client/include/RpcClient.h>


// 集群中的每一个成员(除了本身)会使用Peer来管理

namespace kan {

using tzrpc_client::rpc_handler_t;
using tzrpc_client::RpcClient;


class Peer {

public:
    Peer(uint64_t id,
         const std::string& addr, uint16_t port, const rpc_handler_t& handler);
    ~Peer() = default;

    int send_raft_RPC(uint16_t service_id, uint16_t opcode, const std::string& payload) const;

    int proxy_client_RPC(uint16_t service_id, uint16_t opcode,
                         const std::string& payload, std::string& respload) const;

    std::string str() const {

        std::stringstream ss;
        ss  << "Peer Info: " << std::endl
            << "    id: " << id_ << std::endl
            << "    addr port: " << addr_ << " " << port_ << std::endl
            << "    next_index: " << next_index_ << std::endl
            << "    match_index: " << match_index_ << std::endl
            << "    latest_epoch: " << latest_epoch_ << std::endl;

        return ss.str();
    }

public:

    uint64_t id() const { return id_; }

    uint64_t next_index() const { return next_index_; }
    uint64_t match_index() const { return match_index_; }
    uint64_t latest_epoch() const { return latest_epoch_; }

    void set_next_index(uint64_t index) { next_index_ = index; }
    void set_match_index(uint64_t index) { match_index_ = index; }
    void set_latest_epoch(uint64_t epoch) { latest_epoch_ = epoch; }

private:
    const uint64_t id_;

    const std::string addr_;
    uint16_t port_;
    rpc_handler_t handler_;

    // Raft协议使用的RPC
    std::unique_ptr<RpcClient> rpc_client_;

    // 客户端请求的RPC转发(到Leader)
    std::unique_ptr<RpcClient> rpc_proxy_;

    // 对应于需要发送的下一条日志的索引值，初始化为leader的日志最后索引值+1
    uint64_t next_index_;

    // 已经复制给peer的最高索引值，新选主后会将该值设置为0
    uint64_t match_index_;

    // 发现Peer在安装快照可能会占用不少的时间，
    bool install_snapshot_;

    uint64_t latest_epoch_;

    friend std::ostream& operator<<(std::ostream& os, const Peer& peer);

};

} // namespace kan

#endif // __RAFT_PEER_H__
