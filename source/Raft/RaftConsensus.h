#ifndef __RAFT_RAFT_CONSENSUS_H__
#define __RAFT_RAFT_CONSENSUS_H__

#include <xtra_rhel.h>
#include <thread>


#include <message/ProtoBuf.h>
#include <Protocol/gen-cpp/Raft.pb.h>

#include <Raft/Peer.h>
#include <Raft/LogIf.h>
#include <Raft/Context.h>
#include <Raft/Option.h>
#include <Raft/Clock.h>

#include <Client/include/RpcClientStatus.h>

namespace tzrpc {
class RaftService;
}

namespace sisyphus {

using tzrpc_client::RpcClientStatus;
class Clock;

class RaftConsensus {

    // will access internal do_handle_xxx_request
    friend class tzrpc::RaftService;

    // access internal main_notify_
    friend class Clock;

    __noncopyable__(RaftConsensus)

public:

    typedef sisyphus::Raft::OpCode  OpCode;

    RaftConsensus() :
        consensus_mutex_(),
        peer_set_(),
        log_meta_(),
        option_(),
        context_(),
        main_thread_stop_(false) {
    }

    ~RaftConsensus() {
        if (main_thread_.joinable())
            main_thread_.join();
    }

    bool init();

    // 对于向peer发送的rpc请求，其响应都会在这个函数中异步执行
    int handle_rpc_callback(RpcClientStatus status, uint16_t service_id, uint16_t opcode, const std::string& rsp);

private:

    // 处理异步客户端收到的Peer回调
    int do_handle_request_vote_callback(const Raft::RequestVoteOps::Response& response);
    int do_handle_append_entries_callback(const Raft::AppendEntriesOps::Response& response);
    int do_handle_install_snapshot_callback(const Raft::InstallSnapshotOps::Response& response);

    // 处理Peer发过来的RPC请求
    int do_handle_request_vote_request(const Raft::RequestVoteOps::Request& request, Raft::RequestVoteOps::Response& response);
    int do_handle_append_entries_request(const Raft::AppendEntriesOps::Request& request, Raft::AppendEntriesOps::Response& response);
    int do_handle_install_snapshot_request(const Raft::InstallSnapshotOps::Request& request, Raft::InstallSnapshotOps::Response& response);

    int send_request_vote();
    int send_append_entries();
    int send_append_entries(const Peer& peer);
    int send_install_snapshot();

    void main_thread_run();

private:

    // 实例的全局互斥保护
    std::mutex consensus_mutex_;

    // Timer
    SimpleTimer heartbeat_timer_;
    SimpleTimer election_timer_;
    SimpleTimer withhold_votes_timer_;

    // current static conf, not protected
    std::map<uint64_t, std::shared_ptr<Peer>> peer_set_;

    // Raft log & meta store
    std::unique_ptr<LogIf> log_meta_;

    Option option_;
    std::unique_ptr<Context> context_;

    std::mutex main_mutex_;
    std::condition_variable main_notify_;
    bool main_thread_stop_;
    std::thread main_thread_;
};

} // namespace sisyphus

#endif // __RAFT_RAFT_CONSENSUS_H__
