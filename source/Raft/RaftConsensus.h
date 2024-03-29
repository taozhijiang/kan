/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */


#ifndef __RAFT_RAFT_CONSENSUS_H__
#define __RAFT_RAFT_CONSENSUS_H__

#include <xtra_rhel.h>

#include <thread>

#include <container/EQueue.h>
#include <message/ProtoBuf.h>
#include <concurrency/DeferTask.h>

#include <Protocol/gen-cpp/Raft.pb.h>

#include <Raft/Peer.h>
#include <Raft/LogIf.h>
#include <Raft/Context.h>
#include <Raft/Option.h>
#include <Raft/Clock.h>
#include <Raft/StateMachine.h>
#include <Raft/StoreIf.h>

#include <Client/include/RpcClient.h>
#include <Client/include/RpcClientStatus.h>

namespace tzrpc {
class RaftService;
class ClientService;
}

namespace kan {

using tzrpc_client::RpcClientStatus;
class Clock;

class RaftConsensus {

    // access internal do_handle_xxx_request
    friend class tzrpc::RaftService;

    // access internal kv_store_
    friend class tzrpc::ClientService;

    // access internal consensus_notify_
    friend class Clock;

    __noncopyable__(RaftConsensus)

public:

    typedef kan::Raft::OpCode  OpCode;

    RaftConsensus() :
        consensus_mutex_(),
        consensus_notify_(),
        client_mutex_(),
        client_notify_(),
        peer_set_(),
        log_meta_(),
        option_(),
        context_(),
        main_thread_stop_(false),
        defer_cb_task_() {
    }

    ~RaftConsensus() {
        if (main_thread_.joinable())
            main_thread_.join();

        defer_cb_task_.terminate();
    }

    bool init();

    // 对于向peer发送的rpc请求，其响应都会在这个函数中异步执行
    int handle_rpc_callback(RpcClientStatus status, uint16_t service_id, uint16_t opcode, const std::string& rsp);

    // 暴露给ClientService使用的业务侧接口
    // 如果本机就是Leader，则返回0，否则返回Leader的id
    uint64_t my_id() const;
    uint64_t current_leader() const;
    bool is_leader() const;
    std::shared_ptr<Peer> get_peer(uint64_t peer_id) const;

    // 状态机查询、快照相关的接口
    int state_machine_modify(const std::string& cmd, std::string& apply_out);
    int state_machine_query(const std::string& cmd, std::string& query_out);
    int state_machine_snapshot();

    // 展示集群的状态信息，其中Leader节点的信息较多
    int cluster_stat(std::string& stat);

    void consensus_notify() { consensus_notify_.notify_all(); }
    void client_notify() { client_notify_.notify_all(); }

private:

    // 处理异步客户端收到的Peer回调
    int continue_bf_async(uint16_t opcode, std::string rsp_content);
    int on_request_vote_response_async(const Raft::RequestVoteOps::Response& response);
    int on_append_entries_response_async(const Raft::AppendEntriesOps::Response& response);
    int on_install_snapshot_response_async(const Raft::InstallSnapshotOps::Response& response);

    // 处理Peer发过来的RPC请求
    int on_request_vote_request(const Raft::RequestVoteOps::Request& request,
                                Raft::RequestVoteOps::Response& response);
    int on_append_entries_request(const Raft::AppendEntriesOps::Request& request,
                                  Raft::AppendEntriesOps::Response& response);
    int on_install_snapshot_request(const Raft::InstallSnapshotOps::Request& request,
                                    Raft::InstallSnapshotOps::Response& response);

    // Leader检查cluster的日志状态
    // 当日志复制到绝大多数节点(next_index)的时候，就将其确认为提交的
    uint64_t advance_commit_index() const;

    uint64_t advance_epoch() const;

    int send_request_vote();
    int send_append_entries();
    int send_append_entries(const Peer& peer);
    int send_install_snapshot(const Peer& peer);


private:

    // 实例的全局互斥保护
    // 因为涉及到的模块比较多，所以该锁是一个模块全局性的大锁
    // 另外，锁的特性是不可重入的，所以要避免死锁
    std::mutex consensus_mutex_;
    std::condition_variable consensus_notify_;

    // 用于除上面互斥和信号量保护之外的用途，主要是客户端请求需要
    // 改变状态机的时候
    std::mutex client_mutex_;
    std::condition_variable client_notify_;

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

    // 主线程轮训，主要是根据当前的Role来作出对应决策，比如选举定时器超时等
    bool main_thread_stop_;
    std::thread main_thread_;
    void main_thread_loop();

    // 主要是对于RPC客户端异步回调，如果直接调用会导致io_service的线程阻塞住，
    // 所以对于回调任务，都丢到一个队列中，然后使用这个异步线程执行(因为基本都是加锁了的，所以这边就直接用一个线程)
    roo::DeferTask defer_cb_task_;

    // 状态机处理模块，机器对应的LevelDB底层存储模块
    std::unique_ptr<StateMachine> state_machine_;
    std::unique_ptr<StoreIf>      kv_store_;
};

} // namespace kan

#endif // __RAFT_RAFT_CONSENSUS_H__
