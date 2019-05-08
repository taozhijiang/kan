#ifndef __RAFT_RAFT_CONSENSUS_H__
#define __RAFT_RAFT_CONSENSUS_H__

#include <xtra_rhel.h>

#include <message/ProtoBuf.h>
#include <Protocol/gen-cpp/Raft.pb.h>

#include <Raft/Peer.h>
#include <Raft/LogIf.h>

#include <Client/include/RpcClientStatus.h>

namespace sisyphus {

using tzrpc_client::RpcClientStatus;

class RaftConsensus {

public:

    typedef sisyphus::Raft::OpCode  OpCode;

    RaftConsensus() {
    }

    ~RaftConsensus() {}

    bool init();

    // 对于向peer发送的rpc请求，其响应都会在这个函数中异步执行
    int handle_rpc_callback(RpcClientStatus status, uint16_t service_id, uint16_t opcode, const std::string& rsp);

public:

    int do_handle_request_vote(
        const Raft::RequestVoteOps::Request& request,
        Raft::RequestVoteOps::Response& response);

    int do_handle_append_entries(
        const Raft::AppendEntriesOps::Request& request,
        Raft::AppendEntriesOps::Response& response);

    int do_handle_install_snapshot(
        const Raft::InstallSnapshotOps::Request& request,
        Raft::InstallSnapshotOps::Response& response);


private:

    std::map<uint64_t, std::shared_ptr<Peer>> peer_map_;

    std::shared_ptr<LogIf> log_meta_;
};

} // namespace sisyphus

#endif // __RAFT_RAFT_CONSENSUS_H__
