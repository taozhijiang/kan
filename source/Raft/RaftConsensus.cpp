/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <concurrency/Timer.h>
#include <scaffold/Setting.h>
#include <other/Log.h>
#include <string/StrUtil.h>
#include <other/FilesystemUtil.h>

#include <Protocol/Common.h>

#include <Raft/LevelDBLog.h>
#include <Raft/Clock.h>
#include <Raft/LevelDBStore.h>

#include <Captain.h>

#include "RaftConsensus.h"

namespace sisyphus {


bool RaftConsensus::init() {

    auto setting_ptr = Captain::instance().setting_ptr_->get_setting();
    if (!setting_ptr) {
        roo::log_err("Request setting from Captain failed.");
        return false;
    }

    setting_ptr->lookupValue("Raft.bootstrap", option_.bootstrap_);
    setting_ptr->lookupValue("Raft.server_id", option_.id_);
    setting_ptr->lookupValue("Raft.storage_prefix", option_.log_path_);
    setting_ptr->lookupValue("Raft.log_trans_count", option_.log_trans_count_);

    uint64_t heartbeat_ms;
    uint64_t election_timeout_ms;
    uint64_t raft_distr_timeout_ms;
    setting_ptr->lookupValue("Raft.heartbeat_ms", heartbeat_ms);
    setting_ptr->lookupValue("Raft.election_timeout_ms", election_timeout_ms);
    setting_ptr->lookupValue("Raft.raft_distr_timeout_ms", raft_distr_timeout_ms);
    option_.heartbeat_ms_ = duration(heartbeat_ms);
    option_.election_timeout_ms_ = duration(election_timeout_ms);
    option_.raft_distr_timeout_ms_ = duration(raft_distr_timeout_ms);

    // 作为必填配置参数处理
    if(option_.raft_distr_timeout_ms_.count() == 0)
        option_.raft_distr_timeout_ms_ = option_.election_timeout_ms_;
    
    // if not found, will throw exceptions
    const libconfig::Setting& peers = setting_ptr->lookup("Raft.cluster_peers");
    for (int i = 0; i < peers.getLength(); ++i) {

        uint64_t id;
        std::string addr;
        uint64_t port;
        const libconfig::Setting& peer = peers[i];

        peer.lookupValue("server_id", id);
        peer.lookupValue("addr", addr);
        peer.lookupValue("port", port);

        if (id == 0 || addr.empty() || port == 0) {
            roo::log_err("Find problem peer setting: id %lu, addr %s, port %lu, skip this member.", id, addr.c_str(), port);
            continue;
        }

        if (option_.members_.find(id) != option_.members_.end()) {
            roo::log_err("This node already added before: id %lu, addr %s, port %lu.", id, addr.c_str(), port);
            continue;
        }

        option_.members_[id] = std::make_pair(addr, port);
        option_.members_str_ += roo::StrUtil::to_string(id) + ">" + addr + ":" + roo::StrUtil::to_string(port) + ",";
    }

    option_.withhold_votes_ms_ = option_.election_timeout_ms_;
    if (!option_.validate()) {
        roo::log_err("Validate raft option failed, please check the configuration file!");
        return false;
    }
    roo::log_info("Current setting dump for node %lu:\n %s", option_.id_, option_.str().c_str());

    // 随机化选取超时定时器
    ::srand(::time(NULL) + option_.id_);
    if (option_.election_timeout_ms_.count() > 3)
        option_.election_timeout_ms_ += duration(::random() % (option_.election_timeout_ms_.count() / 3));

    // 初始化 peer_map_ 的主机列表
    for (auto iter = option_.members_.begin(); iter != option_.members_.end(); ++iter) {
        auto endpoint = iter->second;
        auto peer = std::make_shared<Peer>(iter->first, endpoint.first, endpoint.second,
                                           std::bind(&RaftConsensus::handle_rpc_callback, this,
                                                     std::placeholders::_1, std::placeholders::_2,
                                                     std::placeholders::_3, std::placeholders::_4));
        if (!peer) {
            roo::log_err("Create peer member instance %lu failed.", iter->first);
            return false;
        }

        peer_set_[iter->first] = peer;
    }
    roo::log_warning("Totally detected and successfully initialized %lu peers!", peer_set_.size());


    // adjust log store path
    option_.log_path_ += "/instance_" + roo::StrUtil::to_string(option_.id_);
    if (!roo::FilesystemUtil::exists(option_.log_path_)) {
        ::mkdir(option_.log_path_.c_str(), 0755);
        if (!roo::FilesystemUtil::exists(option_.log_path_)) {
            roo::log_err("Create node base storage directory failed: %s.", option_.log_path_.c_str());
            return false;
        }
    }

    log_meta_ = make_unique<LevelDBLog>(option_.log_path_ + "/log_meta");
    if (!log_meta_) {
        roo::log_err("Create LevelDBLog handle failed.");
        return false;
    }

    kv_store_ = make_unique<LevelDBStore>(option_.log_path_ + "/kv_store", option_.log_path_ + "/snapshot/");
    if (!kv_store_) {
        roo::log_err("Create LevelDBStore handle failed.");
        return false;
    }

    // create context
    context_ = make_unique<Context>(option_.id_, log_meta_);
    if (!context_) {
        roo::log_err("Create Raft runtime context failed.");
        return false;
    }

    // 获取meta数据
    LogIf::LogMeta meta;
    log_meta_->meta_data(&meta);
    if (meta.has_current_term())
        context_->set_term(meta.current_term());
    if (meta.has_voted_for())
        context_->set_voted_for(meta.voted_for());
    if (meta.has_commit_index())
        context_->set_commit_index(meta.commit_index());

    context_->update_meta();
    roo::log_warning("startup with meta info, term %lu, voted_for %lu, commit_index %lu",
                     context_->term(), context_->voted_for(), context_->commit_index());

    // bootstrap ???
    if (option_.bootstrap_) {
        roo::log_warning("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        roo::log_warning("bootstrap operation here ...");
        roo::log_warning("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        if (context_->term() != 0 ||
            log_meta_->last_index() != 0 ||
            log_meta_->start_index() != 1) {
            roo::log_err("not suitable bootstrap with: %s", context_->str().c_str());
            ::exit(EXIT_FAILURE);
        }

        // 模拟构建一个日志
        context_->become_follower(1);

        LogIf::EntryPtr entry = std::make_shared<LogIf::Entry>();
        entry->set_term(context_->term());
        entry->set_type(Raft::EntryType::kNoop);
        log_meta_->append({ entry });

        ::sleep(1);
        roo::log_warning("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        roo::log_warning("bootstrap finished, please turn off bootstrap flag and start this member again!");
        roo::log_warning("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        ::exit(EXIT_SUCCESS);
    }

    // 状态机执行线程
    state_machine_ = make_unique<StateMachine>(*this, log_meta_, kv_store_);
    if (!state_machine_ || !state_machine_->init()) {
        roo::log_err("Create and initialize StateMachine failed.");
        return false;
    }

    // 主工作线程
    main_thread_ = std::thread(std::bind(&RaftConsensus::main_thread_loop, this));

    // 启动选取定时器
    context_->become_follower(context_->term());
    election_timer_.schedule();

    roo::log_warning("Member %lu successfully initialized.", my_id());
    return true;
}


uint64_t RaftConsensus::my_id() const {
    return context_->id();
}

uint64_t RaftConsensus::current_leader() const {
    return context_->leader_id();
}

bool RaftConsensus::is_leader() const {
    return context_->id() == context_->leader_id();
}

int RaftConsensus::cluster_stat(std::string& stat) {
    std::stringstream ss;

    ss << std::endl
       << "cluster stat current node: " << std::endl
       << context_->str() << std::endl
       << option_.str() << std::endl;

    if (is_leader()) {
        for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter) {
            ss << std::endl
                << iter->second->str() << std::endl;
        }
    }

    stat = ss.str();
    return 0;
}


std::shared_ptr<Peer> RaftConsensus::get_peer(uint64_t peer_id) const {
    auto peer = peer_set_.find(peer_id);
    if (peer == peer_set_.end()) {
        roo::log_err("Bad, request peer_id's id(%lu) is not in peer_set!", peer_id);
        return{ };
    }

    return peer->second;
}

// 不能持锁，否则快照时间过长会导致各种定时器超时
// 这边还是手动触发执行快照的，实测在200W条日志压缩后删除的时候，大概需要3s的时长，这
// 会触发超时，导致快照的机器失去Leader
int RaftConsensus::state_machine_snapshot() {

    uint64_t last_included_index = 0;
    uint64_t last_included_term  = 0;

    if (!state_machine_->create_snapshot(last_included_index, last_included_term)) {
        roo::log_err("Create Snapshot failed.");
        return -1;
    }

    roo::log_warning("Create Snapshot successfully at index %lu, term %lu.",
                     last_included_index, last_included_term);

    // 更新context_
    {
        std::unique_lock<std::mutex> lock(consensus_mutex_);

        context_->set_last_included_index(last_included_index);
        context_->set_last_included_term(last_included_term);

        // trim日志优化
        roo::log_warning("truncate_prefix of log from index: %lu.", last_included_index);
        log_meta_->truncate_prefix(last_included_index + 1);
        roo::log_warning("current start_index %lu, last_index %lu.", log_meta_->start_index(), log_meta_->last_index());
    }

    return 0;
}


// 必须确保当前是有效的Leader节点，同时取到请求时候的最新commit_index，
// 依据该commit_index取出apply_index后对应的执行结果，得到的内容才能够
// 保证是线性一致性的
int RaftConsensus::state_machine_query(const std::string& cmd, std::string& query_out) {

    if (cmd.empty()) {
        roo::log_err("StateMachineSelect cmd is empty.");
        return -1;
    }

    if (!is_leader()) {
        roo::log_err("Current leader is %lu, this node can not handle this request.", current_leader());
        return -1;
    }

    uint64_t aim_epoch = context_->inc_epoch();
    uint64_t aim_index = 0;
    uint64_t aim_term  = 0;

    // 设置该标志后，main_loop会紧急调用send_append_entries()，就会启动一次心跳请求
    heartbeat_timer_.set_urgent();
    consensus_notify_.notify_all();

    {

        auto expire_tp = steady_clock::now();
        if (option_.raft_distr_timeout_ms_.count() != 0)
            expire_tp += std::chrono::milliseconds(option_.raft_distr_timeout_ms_);
        else
            expire_tp += std::chrono::milliseconds(std::numeric_limits<int32_t>::max());

        std::unique_lock<std::mutex> lock(client_mutex_);

        // no_timeout wakeup by notify_all, notify_one, or spuriously
        // timeout    wakeup by timeout expiration
        while (advance_epoch() < aim_epoch) {

#if __cplusplus >= 201103L
            if (client_notify_.wait_until(lock, expire_tp) == std::cv_status::timeout) {
                break;
            }
#else
            if (!client_notify_.wait_until(lock, expire_tp)) {
                break;
            }
#endif
        }


        aim_index = context_->commit_index();
        aim_term  = 0;
        if (context_->commit_index() == context_->last_included_index()) {
            aim_term = context_->last_included_term();
        } else {
            auto entry = log_meta_->entry(context_->commit_index());
            aim_term = entry->term();
        }

        // 比如重新发生了选举操作
        if (aim_term != context_->term()) {
            roo::log_err("Term changed when epoch check, previous %lu, current %lu", aim_term, context_->term());
            return -1;
        }

        roo::log_info("up-to-date leader, epoch %lu, and aim_term %lu aim_index %lu.", aim_epoch, aim_term, aim_index);
    }

    {
        // we are the up-to-date leader
        // wait until apply_index come to the commit_index

        auto expire_tp = steady_clock::now();
        if (option_.raft_distr_timeout_ms_.count() != 0)
            expire_tp += std::chrono::milliseconds(option_.raft_distr_timeout_ms_);
        else
            expire_tp += std::chrono::milliseconds(std::numeric_limits<int32_t>::max());


        std::unique_lock<std::mutex> lock(client_mutex_);
        // no_timeout wakeup by notify_all, notify_one, or spuriously
        // timeout    wakeup by timeout expiration
        while (state_machine_->apply_index() < aim_index) {

#if __cplusplus >= 201103L
            if (client_notify_.wait_until(lock, expire_tp) == std::cv_status::timeout) {
                break;
            }
#else
            if (!client_notify_.wait_until(lock, expire_tp)) {
                break;
            }
#endif
        }

        if (state_machine_->apply_index() < aim_index) {
            roo::log_err("wait for state_machine apply entry %lu about %lu msec(s) timeout happens.", 
                         aim_index, option_.raft_distr_timeout_ms_.count());
            return -1;
        }
    }

    Client::StateMachineSelectOps::Request  request;
    if (!roo::ProtoBuf::unmarshalling_from_string(cmd, &request)) {
        roo::log_err("unmarshal request failed.");
        return -1;
    }

    Client::StateMachineSelectOps::Response response;
    int ret = kv_store_->select_handle(request, response);
    if (ret != 0) {
        roo::log_err("kv store do query failed with %d.", ret);
        return ret;
    }

    response.set_code(0);
    response.set_msg("OK");
    roo::ProtoBuf::marshalling_to_string(response, &query_out);

    return 0;
}

int RaftConsensus::state_machine_modify(const std::string& cmd, std::string& apply_out) {

    if (cmd.empty()) {
        roo::log_err("StateMachineUpdate cmd is empty.");
        return -1;
    }

    if (!is_leader()) {
        roo::log_err("Current leader is %lu, this node can not handle this request.", current_leader());
        return -1;
    }

    // 改条请求对应的日志索引
    uint64_t aim_term  = 0;
    uint64_t aim_index = 0;

    {
        // 创建附带状态机变更指令的业务日志RPC
        LogIf::EntryPtr entry = std::make_shared<LogIf::Entry>();
        entry->set_term(context_->term());
        entry->set_type(Raft::EntryType::kNormal);
        entry->set_data(cmd);

        std::lock_guard<std::mutex> lock(consensus_mutex_);
        log_meta_->append({ entry });
        auto term_and_index = log_meta_->last_term_and_index();
        aim_term  = term_and_index.first;
        aim_index = term_and_index.second;
    }
    roo::log_info("The request aim for index: %lu", aim_index);


    // 设置该标志后，main_loop会紧急调用send_append_entries()
    heartbeat_timer_.set_urgent();
    consensus_notify_.notify_all();

    {

        auto expire_tp = steady_clock::now();
        if (option_.raft_distr_timeout_ms_.count() != 0)
            expire_tp += std::chrono::milliseconds(option_.raft_distr_timeout_ms_);
        else
            expire_tp += std::chrono::milliseconds(std::numeric_limits<int32_t>::max());

        std::unique_lock<std::mutex> lock(client_mutex_);

        // no_timeout wakeup by notify_all, notify_one, or spuriously
        // timeout    wakeup by timeout expiration
        while (context_->commit_index() < aim_index && aim_term == context_->term()) {

#if __cplusplus >= 201103L
            if (client_notify_.wait_until(lock, expire_tp) == std::cv_status::timeout) {
                break;
            }
#else
            if (!client_notify_.wait_until(lock, expire_tp)) {
                break;
            }
#endif
        }

        // 重新发生了选举操作
        if (aim_term != context_->term()) {
            roo::log_err("Term changed when replicate entry, previous %lu, current %lu", aim_term, context_->term());
            return -1;
        }

        if (context_->commit_index() < aim_index) {
            roo::log_err("replicate entry %lu about %lu msec(s) timeout happens.", 
                         aim_index, option_.raft_distr_timeout_ms_.count());
            return -1;
        }
    }

    {
        auto expire_tp = steady_clock::now();
        if (option_.raft_distr_timeout_ms_.count() != 0)
            expire_tp += std::chrono::milliseconds(option_.raft_distr_timeout_ms_);
        else
            expire_tp += std::chrono::milliseconds(std::numeric_limits<int32_t>::max());


        std::unique_lock<std::mutex> lock(client_mutex_);

        // no_timeout wakeup by notify_all, notify_one, or spuriously
        // timeout    wakeup by timeout expiration
        while (state_machine_->apply_index() < aim_index) {

#if __cplusplus >= 201103L
            if (client_notify_.wait_until(lock, expire_tp) == std::cv_status::timeout) {
                break;
            }
#else
            if (!client_notify_.wait_until(lock, expire_tp)) {
                break;
            }
#endif
        }

        if (state_machine_->apply_index() < aim_index) {
            roo::log_err("entry %lu about %lu msec(s) timeout happens.", aim_index, option_.raft_distr_timeout_ms_.count());
            return -1;
        }
    }

    // 获取状态机执行的缓存结果
    Client::StateMachineUpdateOps::Response response;
    response.set_code(0);
    response.set_msg("OK");

    std::string content;
    if (state_machine_->fetch_response_msg(aim_index, content))
        response.set_context(content);

    roo::ProtoBuf::marshalling_to_string(response, &apply_out);
    return 0;
}

int RaftConsensus::handle_rpc_callback(RpcClientStatus status, uint16_t service_id, uint16_t opcode, const std::string& rsp) {

    if (status != RpcClientStatus::OK) {
        roo::log_err("RPC call failed with status code %d, for service_id %u and opcode %u.",
                     static_cast<uint8_t>(status), service_id, opcode);
        return -1;
    }

    if (service_id != static_cast<uint16_t>(tzrpc::ServiceID::RAFT_SERVICE)) {
        roo::log_err("Recived callback with invalid service_id %u, expect %d",
                     service_id, tzrpc::ServiceID::RAFT_SERVICE);
        return -1;
    }

    // 解析异步响应报文，分发执行相应的消息处理
    if (opcode == static_cast<uint16_t>(OpCode::kRequestVote)) {
        Raft::RequestVoteOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("ProtoBuf unmarshal RequestVoteOps response failed.");
            return -1;
        }
        return continue_request_vote_bf_async(response);
    } else if (opcode == static_cast<uint16_t>(OpCode::kAppendEntries)) {
        Raft::AppendEntriesOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("ProtoBuf unmarshal AppendEntriesOps response failed.");
            return -1;
        }
        return continue_append_entries_bf_async(response);
    } else if (opcode == static_cast<uint16_t>(OpCode::kInstallSnapshot)) {
        Raft::InstallSnapshotOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("ProtoBuf unmarshal InstallSnapshotOps response failed.");
            return -1;
        }
        return continue_install_snapshot_bf_async(response);
    }

    roo::log_err("Unexpected RPC call response with opcode %u", opcode);
    return -1;
}


//
// 响应请求
//
int RaftConsensus::handle_request_vote_request(const Raft::RequestVoteOps::Request& request,
                                               Raft::RequestVoteOps::Response& response) {

    std::lock_guard<std::mutex> lock(consensus_mutex_);

    response.set_peer_id(context_->id());
    response.set_epoch(request.epoch());

    auto last_entry_term_index = log_meta_->last_term_and_index();
    bool log_is_ok = (request.last_log_term() > last_entry_term_index.first) ||
        (request.last_log_term() == last_entry_term_index.first && request.last_log_index() >= last_entry_term_index.second);

    // 节点在选取超时内接收到另外一个Leader的AppendEntries，则拒绝本轮的选取请求
    // 这样可以避免某些Peer自身的原因导致意外的选主请求
    if (withhold_votes_timer_.within(option_.withhold_votes_ms_)) {
        roo::log_warning("RequestVote reject, because this node heard anthor leader AppendEntries within ElectionTimeout ...");
        response.set_term(context_->term());
        response.set_vote_granted(false);
        response.set_log_ok(log_is_ok);
        return 0;
    }

    if (request.term() > context_->term()) {
        roo::log_warning("Found larger term %lu from %lu compared with our %lu, step to it.",
                         request.term(), request.candidate_id(), context_->term());
        context_->become_follower(request.term());
    }

    if (request.term() < context_->term()) {
        roo::log_warning("RequestVote reject, because small term %lu compared with our %lu.",
                         request.term(), context_->term());
        response.set_term(context_->term());
        response.set_vote_granted(false);
        response.set_log_ok(log_is_ok);
        return 0;
    }

    // 第一次投票，记录下voted_for字段
    if (log_is_ok && context_->voted_for() == 0) {
        context_->become_follower(request.term());
        heartbeat_timer_.disable();
        election_timer_.schedule();
        context_->set_voted_for(request.candidate_id());
        context_->update_meta();
    }

    response.set_term(context_->term());
    bool voted_granted = log_is_ok && context_->term() == request.term() && context_->voted_for() == request.candidate_id();
    response.set_vote_granted(voted_granted);
    response.set_log_ok(log_is_ok);

    roo::log_warning("RequestVote %s with log_ok: %d", voted_granted ? "voted" : "reject", log_is_ok);
    return 0;
}

int RaftConsensus::handle_append_entries_request(const Raft::AppendEntriesOps::Request& request,
                                                 Raft::AppendEntriesOps::Response& response) {

    std::lock_guard<std::mutex> lock(consensus_mutex_);

    response.set_peer_id(option_.id_);
    response.set_epoch(request.epoch());

    response.set_term(context_->term());
    response.set_success(false);
    response.set_last_log_index(log_meta_->last_index());

    if (request.term() < context_->term()) {
        roo::log_warning("AppendEntriesOps failed, recevied smaller term %lu compared with our %lu.",
                         request.term(), context_->term());
        response.set_success(false);
        return 0;
    }

    if (request.term() > context_->term()) {
        roo::log_warning("Found larger term %lu compared with our %lu, step to it.", request.term(), context_->term());

        // bump up our term
        context_->set_term(request.term());
        context_->update_meta();
        response.set_term(context_->term());
    }

    context_->become_follower(request.term());
    heartbeat_timer_.disable();
    election_timer_.schedule();
    withhold_votes_timer_.schedule();

    // 更新本地leader_id
    if (context_->leader_id() == 0) {
        context_->set_leader_id(request.leader_id());
    }

    if (request.prev_log_index() > log_meta_->last_index()) {
        roo::log_warning("AppendEntries failed, received log_index %lu is too new compared with our %lu.",
                         request.prev_log_index(), log_meta_->last_index());
        response.set_success(false);
        return 0;
    }

    // 检查，保证前一条日志的term必须匹配，为了安全性考虑
    if (request.prev_log_index() >= log_meta_->start_index() &&
        log_meta_->entry(request.prev_log_index())->term() != request.prev_log_term()) {
        roo::log_err("Previous log index %lu with term %lu not match with our %lu, "
                     "so we reject this entry, and leader will override it later!",
                     request.prev_log_index(),
                     request.prev_log_term(), log_meta_->entry(request.prev_log_index())->term());
        log_meta_->truncate_suffix(log_meta_->last_index() -1);
        response.set_last_log_index(log_meta_->last_index());

        response.set_success(false);
        return 0;
    }

    // 目前为止都好，将日志添加到本地，并更新响应索引
    response.set_success(true);


    uint64_t index = request.prev_log_index();
    for (auto iter = request.entries().begin(); iter != request.entries().end(); ++iter) {
        ++index;

        const Raft::Entry& entry = *iter;
        if (index < log_meta_->start_index())
            continue;

        // 某些日志已经存在，则检查他们的term是否一致
        if (log_meta_->last_index() >= index) {
            if (log_meta_->entry(index)->term() == entry.term())
                continue;

            // 发现相同index的日志的term不一致，则清除这些日志，等待leader重发
            roo::log_err("find index %lu log has different term %lu %lu, truncate from here",
                         index, log_meta_->entry(index)->term(), entry.term());

            uint64_t log_kept = index - 1;
            log_meta_->truncate_suffix(log_kept);
        }

        // 重叠的日志已经检查过了，这边进行实际要追加日志的操作
        std::vector<LogIf::EntryPtr> entries;
        do {
            auto entry = std::make_shared<LogIf::Entry>(*iter);
            entries.emplace_back(entry);
            ++iter;
            ++index;
        } while (iter != request.entries().end());

        roo::log_warning("Will totally append %lu log_entry from %lu.", entries.size(), log_meta_->last_index());
        log_meta_->append(entries);
        break;
    }

    response.set_last_log_index(log_meta_->last_index());

    // Leader在设置该参数的时候，就确保了该值肯定不会超过该Peer的日志范围的
    if (context_->commit_index() < request.leader_commit()) {
        context_->set_commit_index(request.leader_commit());
        log_meta_->set_meta_commit_index(context_->commit_index());
        this->client_notify();
    }

    return 0;
}

int RaftConsensus::handle_install_snapshot_request(const Raft::InstallSnapshotOps::Request& request,
                                                   Raft::InstallSnapshotOps::Response& response) {

    std::lock_guard<std::mutex> lock(consensus_mutex_);

    response.set_peer_id(option_.id_);
    response.set_epoch(request.epoch());

    response.set_term(context_->term());

    if (request.term() < context_->term()) {
        roo::log_warning("InstallSnapshot failed, recevied smaller term %lu compared with our %lu.",
                         request.term(), context_->term());
        return 0;
    }

    if (request.term() > context_->term()) {
        roo::log_warning("Found larger term %lu compared with our %lu, step to it.", request.term(), context_->term());

        // bump up our term
        context_->set_term(request.term());
        context_->update_meta();
        
        response.set_term(context_->term());
    }

    context_->become_follower(request.term());
    heartbeat_timer_.disable();
    election_timer_.schedule();
    withhold_votes_timer_.schedule();

    // 更新本地leader_id
    if (context_->leader_id() == 0) {
        context_->set_leader_id(request.leader_id());
    }

    std::string content = request.data();
    uint64_t last_included_index = request.last_included_index();
    //   uint64_t last_included_term  = request.last_included_term();

    Snapshot::SnapshotContent snapshot;
    if (!roo::ProtoBuf::unmarshalling_from_string(content, &snapshot)) {
        roo::log_err("unmarshalling snapshot failed, may content currputed.");
        return -1;
    }

    if (!state_machine_->apply_snapshot(snapshot)) {
        roo::log_err("StateMachine apply snapshot failed.");
        return -1;
    }

    // 处理snapshot中的日志覆盖和本地已经有的日志之间的关系
    // 最简单的处理方式，就是丢弃本地所有的日志...
    // 会自动设置start_index和last_index

    roo::log_warning("before install_snapshot, currently start_index %lu, last_index %lu.",
                     log_meta_->start_index(), log_meta_->last_index());

    log_meta_->truncate_prefix(last_included_index + 1);
    log_meta_->truncate_suffix(last_included_index);

    roo::log_warning("after install_snapshot at %lu, finally start_index %lu, last_index %lu.",
                     last_included_index, log_meta_->start_index(), log_meta_->last_index());

    // 设置提交索引
    context_->set_commit_index(last_included_index);
    log_meta_->set_meta_commit_index(last_included_index);
    log_meta_->set_meta_apply_index(last_included_index);
    state_machine_->set_apply_index(last_included_index);

    this->client_notify();

    response.set_bytes_stored(content.size());

    roo::log_warning("StateMachine apply snapshot successfully.");
    return 0;
}


int RaftConsensus::continue_request_vote_bf_async(const Raft::RequestVoteOps::Response& response) {

    std::lock_guard<std::mutex> lock(consensus_mutex_);

    // 如果发现更大的term返回，则回退到follower
    if (response.term() > context_->term()) {

        roo::log_warning("RequestVote received higher term %lu compared with our %lu, rollback to fellower.",
                         response.term(), context_->term());
        context_->become_follower(response.term());

        // update meta info
        LogIf::LogMeta meta;
        meta.set_current_term(response.term());
        meta.set_voted_for(response.peer_id());
        log_meta_->set_meta_data(meta);
        return 0;
    }

    auto iter = peer_set_.find(response.peer_id());
    if (iter == peer_set_.end()) {
        std::string msg = roo::va_format("RequestVote received vote from peer %lu out of cluster.", response.peer_id());
        PANIC(msg.c_str());
    }

    auto peer_ptr = iter->second;
    peer_ptr->set_latest_epoch(response.epoch());
    this->client_notify();

    // 候选者，检查选取结果
    if (context_->role() == Role::kCandidate) {

        if (response.vote_granted() == true) {

            context_->add_quorum_granted(peer_ptr->id());

            // 选举成功
            if (context_->quorum_count() > (option_.members_.size() + 1) / 2) {

                roo::log_warning("Node %lu has enough votes %lu, and will become leader ...",
                                 context_->id(), context_->quorum_count());
                context_->become_leader();

                // 创建空的追加日志RPC
                LogIf::EntryPtr entry = std::make_shared<LogIf::Entry>();
                entry->set_term(context_->term());
                entry->set_type(Raft::EntryType::kNoop);
                auto idx = log_meta_->append({ entry });
                roo::log_info("Current log entries exists from start_index %lu, last_index %lu", idx.first, idx.second);

                // 更新每个peer的数据
                for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter) {
                    iter->second->set_next_index(log_meta_->last_index() + 1);
                    iter->second->set_match_index(0);
                }

                // 选举完成，切换成心跳定时器，关闭选取定时器
                election_timer_.disable();
                withhold_votes_timer_.disable();
                heartbeat_timer_.schedule();

                // 调度，根据Peer的next_index来发送
                heartbeat_timer_.set_urgent();
                consensus_notify_.notify_all();
                // send_append_entries();

                roo::log_warning("Node %lu now has been leader.", context_->id());

                return 0;
            }
        } else {
            // rejected by others
            roo::log_warning("RequestVote node %lu received response, but rejected by peer %lu.",
                             context_->id(), response.peer_id());
            return 0;
        }
    }

    // 其他角色收到了RequestVote的响应
    roo::log_warning("Node %lu received RequestVote response, but its already as %s so ignore it!",
                     my_id(), RoleStr(context_->role()).c_str());
    return 0;
}


// 当日志被复制到绝大多数节点上的时候，我们就将其设置为已提交的状态
// 这边就是根据每个Peer的match_index来尝试算出最新的commit_index
uint64_t RaftConsensus::advance_commit_index() const {

    std::vector<uint64_t> values{};
    for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter)
        values.push_back(iter->second->match_index());

    values.emplace_back(log_meta_->last_index());
    std::sort(values.begin(), values.end());

    return values.at((peer_set_.size() + 1 - 1) / 2);
}

// 保证线性一致性读的epoch
uint64_t RaftConsensus::advance_epoch() const {

    std::vector<uint64_t> values{};
    for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter)
        values.push_back(iter->second->latest_epoch());

    values.emplace_back(context_->epoch());
    std::sort(values.begin(), values.end());

    return values.at((peer_set_.size() + 1 - 1) / 2);
}

int RaftConsensus::continue_append_entries_bf_async(const Raft::AppendEntriesOps::Response& response) {

    std::lock_guard<std::mutex> lock(consensus_mutex_);

    // 如果发现更大的term返回，则回退到follower
    if (response.term() > context_->term()) {

        roo::log_warning("AppendEntries received higher term %lu compared with our %lu, rollback to fellower.",
                         response.term(), context_->term());
        context_->become_follower(response.term());

        // update meta info
        LogIf::LogMeta meta;
        meta.set_current_term(response.term());
        meta.set_voted_for(response.peer_id());
        log_meta_->set_meta_data(meta);
        return 0;
    }

    // 原始协议应该是prev_log_index + numEntries，但是这边没有原始调用的信息，所以
    // 就按照返回的last_log_index来设置了
    if (!response.has_last_log_index()) {
        PANIC("Our implementation, AppendEntries response required last_log_index!");
    }

    auto iter = peer_set_.find(response.peer_id());
    if (iter == peer_set_.end()) {
        std::string msg = roo::va_format("AppendEntries received response outside of cluster with id %lu.", response.peer_id());
        PANIC(msg.c_str());
    }
    auto peer_ptr = iter->second;
    peer_ptr->set_latest_epoch(response.epoch());
    this->client_notify();

    if (response.success() == true) {

        // 日志追加成功，更新对应节点的next_index和match_index的索引
        uint64_t old_match_index = peer_ptr->match_index();
        uint64_t old_next_index  = peer_ptr->next_index();
        peer_ptr->set_match_index(response.last_log_index());
        peer_ptr->set_next_index(peer_ptr->match_index() + 1);
        roo::log_warning("Peer %lu set match_index and next_index from %lu,%lu to %lu,%lu",
                         peer_ptr->id(), old_match_index, old_next_index, peer_ptr->match_index(), peer_ptr->next_index());

        // 尝试计算新的提交日志索引
        uint64_t new_commit_index = advance_commit_index();

        // 重新选主之后，所有的peer的match_index都会被重置为0，所以这边是可能
        // 出现新算出来的提交日志索引比之前的提交日志索引值低的情况
        // 因为我们永远不会修改已经提交的日志，所以如果算出来的索引值回退了，我们
        // 不做更新处理就可以了
        if (new_commit_index <= context_->commit_index()) {
            roo::log_warning("advanced new commit_index %lu backward with current %lu, ignore it!",
                             new_commit_index, context_->commit_index());
            return 0;
        }

        // 这个地方可能会涉及到系统安全，TODO
        if (log_meta_->entry(new_commit_index)->term() != context_->term()) {
            roo::log_warning("new commit index term doesnot agree %lu %lu",
                             log_meta_->entry(new_commit_index)->term(), context_->term());
            return 0;
        }

        if (new_commit_index != context_->commit_index()) {
            roo::log_warning("Leader %lu will advance commit_index from %lu to %lu",
                             my_id(), context_->commit_index(), new_commit_index);
            context_->set_commit_index(new_commit_index);

            // 持久化提交索引
            log_meta_->set_meta_commit_index(context_->commit_index());
            state_machine_->notify_state_machine();

            // 通知等待的客户端
            this->client_notify();
        }

        return 0;
    }

    // response.success() == false

    // 默认情况是依次递减来尝试的
    // if (peer_ptr->next_index() > 1)
    //    peer_ptr->set_next_index(peer_ptr->next_index() - 1);

    // 如果响应返回了last_log_index，则使用这个提示值
    // 在我们的实现中，last_log_index是必须返回的值，所以这边应该是没问题的

    if (peer_ptr->next_index() > response.last_log_index() + 1) {
        peer_ptr->set_next_index(response.last_log_index() + 1);
    }

    // 日志不匹配，减少next_index_然后重发
    roo::log_err("AppendEntries for Peer %lu failed, will try send log entries from %lu.",
                 response.peer_id(), iter->second->next_index());

    // schedule AppendEntries RPC again for specific Peer
    send_append_entries(*peer_ptr);

    return 0;
}


int RaftConsensus::continue_install_snapshot_bf_async(const Raft::InstallSnapshotOps::Response& response) {

    std::lock_guard<std::mutex> lock(consensus_mutex_);

    // 如果发现更大的term返回，则回退到follower
    if (response.term() > context_->term()) {

        roo::log_warning("RequestVote received higher term %lu compared with our %lu, rollback to fellower.",
                         response.term(), context_->term());
        context_->become_follower(response.term());

        // update meta info
        LogIf::LogMeta meta;
        meta.set_current_term(response.term());
        meta.set_voted_for(response.peer_id());
        log_meta_->set_meta_data(meta);
        return 0;
    }

    auto iter = peer_set_.find(response.peer_id());
    if (iter == peer_set_.end()) {
        std::string msg = roo::va_format("InstallSnapshot received response outside of cluster with id %lu.", response.peer_id());
        PANIC(msg.c_str());
    }
    auto peer_ptr = iter->second;
    peer_ptr->set_latest_epoch(response.epoch());
    this->client_notify();

    // 传输成功，执行和append_entries一样的响应处理
    if (response.has_bytes_stored()) {

        uint64_t old_match_index = peer_ptr->match_index();
        uint64_t old_next_index  = peer_ptr->next_index();

        // 设置peer对应的next_index和match_index
        peer_ptr->set_match_index(context_->last_included_index());
        peer_ptr->set_next_index(context_->last_included_index() + 1);

        roo::log_warning("Peer %lu set next_index and commit_index from %lu,%lu to %lu,%lu",
                         peer_ptr->id(), old_match_index, old_next_index, peer_ptr->match_index(), peer_ptr->next_index());

        // 尝试计算新的提交日志索引
        uint64_t new_commit_index = advance_commit_index();

        // 重新选主之后，所有的peer的match_index都会被重置为0，所以这边是可能
        // 出现新算出来的提交日志索引比之前的提交日志索引值低的情况
        // 因为我们永远不会修改已经提交的日志，所以如果算出来的索引值回退了，我们
        // 不做更新处理就可以了
        if (new_commit_index <= context_->commit_index()) {
            roo::log_warning("advanced new commit_index %lu backward with current %lu, ignore it!",
                             new_commit_index, context_->commit_index());
            return 0;
        }

        // 这个地方可能会涉及到系统安全，TODO
        if (log_meta_->entry(new_commit_index)->term() != context_->term()) {
            roo::log_warning("new commit index term doesnot agree %lu %lu",
                             log_meta_->entry(new_commit_index)->term(), context_->term());
            return 0;
        }

        if (new_commit_index != context_->commit_index()) {
            roo::log_warning("Leader %lu will advance commit_index from %lu to %lu",
                             my_id(), context_->commit_index(), new_commit_index);
            context_->set_commit_index(new_commit_index);

            // 持久化提交索引
            log_meta_->set_meta_commit_index(context_->commit_index());
            state_machine_->notify_state_machine();

            // 通知等待的客户端
            this->client_notify();
        }

        return 0;

    }

    roo::log_err("Peer %lu handle install_snapshot failed.", response.peer_id());
    return -1;
}



int RaftConsensus::send_request_vote() {

    // 构造请求报文
    Raft::RequestVoteOps::Request request;
    request.set_term(context_->term());
    request.set_candidate_id(context_->id());

    auto last_entry_term_index = log_meta_->last_term_and_index();
    request.set_last_log_term(last_entry_term_index.first);
    request.set_last_log_index(last_entry_term_index.second);
    request.set_epoch(context_->epoch());

    roo::log_info("RequestVote RPC, term %lu, candidate_id %lu, last_log_term %lu, last_log_index %lu.",
                  request.term(), request.candidate_id(), request.last_log_term(), request.last_log_index());

    std::string str_request;
    roo::ProtoBuf::marshalling_to_string(request, &str_request);

    for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter) {
        iter->second->send_raft_RPC(tzrpc::ServiceID::RAFT_SERVICE, Raft::OpCode::kRequestVote, str_request);
    }

    return 0;
}


int RaftConsensus::send_append_entries() {

    for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter) {
        send_append_entries(*iter->second);
    }

    return 0;
}

int RaftConsensus::send_append_entries(const Peer& peer) {

    std::vector<LogIf::EntryPtr> entries;

    // 无论如何，send_append_entries都要发送，否则Leader无法维持心跳阻止其他Peer选主
    // 对于心跳作用，其RPC的entries条目为空

    uint64_t prev_log_index = peer.next_index() - 1;
    uint64_t prev_log_term = 0;

    if (prev_log_index >= log_meta_->start_index()) {
        LogIf::EntryPtr prev_log = log_meta_->entry(prev_log_index);
        if (!prev_log) {
            roo::log_err("Current Leader's last_log_index %lu, and Peer %lu next_id %lu, match_index %lu.",
                         log_meta_->last_index(), peer.id(), peer.next_index(), peer.match_index());
            roo::log_err("Get entry %lu failed.", prev_log_index);
            return -1;
        }

        prev_log_term = prev_log->term();
    } else if (prev_log_index == 0) {
        prev_log_term = 0;
    } else if (prev_log_index == context_->last_included_index()) {
        prev_log_term = context_->last_included_term();
    } else {
        send_install_snapshot(peer);
        return 0;
    }

    Raft::AppendEntriesOps::Request request;
    request.set_term(context_->term());
    request.set_leader_id(context_->id());
    request.set_prev_log_term(prev_log_term);
    request.set_prev_log_index(prev_log_index);
    request.set_epoch(context_->epoch());

    roo::log_info("AppendEntries RPC, term %lu, leader_id %lu, prev_log_term %lu, prev_log_index %lu.",
                  request.term(), context_->id(), request.prev_log_term(), request.prev_log_index());

    // 可以为空，此时为纯粹的心跳
    // TODO: 限制每次发送的日志条目数
    if (!log_meta_->entries(peer.next_index(), entries, option_.log_trans_count_)) {
        roo::log_err("Get entries from %lu from index %lu failed.", peer.id(), prev_log_index);
        return -1;
    }

    google::protobuf::RepeatedPtrField<Raft::Entry>& request_entries = *request.mutable_entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        *request_entries.Add() = *(entries[i]);
    }

    // 确保commit_index的日志在Peer一定存在
    request.set_leader_commit(std::min(context_->commit_index(), prev_log_index + entries.size()));
    roo::log_info("AppendEntries RPC to Peer %lu, with entries from %lu, size %u, commit_index %lu",
                  peer.id(), prev_log_index, request.entries().size(), request.leader_commit());

    std::string str_request;
    roo::ProtoBuf::marshalling_to_string(request, &str_request);
    peer.send_raft_RPC(tzrpc::ServiceID::RAFT_SERVICE, Raft::OpCode::kAppendEntries, str_request);

    return 0;
}

// 发送安装日志的请求只会向特定的节点发送
int RaftConsensus::send_install_snapshot(const Peer& peer) {

    // 构造请求报文
    Raft::InstallSnapshotOps::Request request;

    request.set_term(context_->term());
    request.set_leader_id(context_->id());

    // 加载快照信息
    std::string content;
    uint64_t last_included_index = 0;
    uint64_t last_included_term  = 0;
    if (!state_machine_->load_snapshot(content, last_included_index, last_included_term)) {
        roo::log_err("load_snapshot failed.");
        return -1;
    }

    // 校验context_中保留的last_included信息和实际snapshot文件中的last_included信息
    if (last_included_index != context_->last_included_index() ||
        last_included_term  != context_->last_included_term()) {
        roo::log_err("snapshot file meta data doesnot match with context info.");
        roo::log_err("snapshot index %lu, term %lu; context index %lu, term %lu.",
                     last_included_index, last_included_term, context_->last_included_index(), context_->last_included_term());

        context_->set_last_included_index(last_included_index);
        context_->set_last_included_term(last_included_term);
    }

    // start_index和last_include中的日志覆盖不能有间隙
    // 正常处理逻辑不应该有这种数据不一致的情况
    if (context_->last_included_index() < log_meta_->start_index() - 1) {
        std::string msg = roo::va_format("Invalid snapshot %lu and start_index %lu.",
                                         context_->last_included_index(), log_meta_->start_index());
        PANIC(msg.c_str());
    }

    request.set_data(content);
    request.set_last_included_index(last_included_index);
    request.set_last_included_term(last_included_term);
    request.set_epoch(context_->epoch());

    roo::log_info("InstallSnapshot RPC, term %lu, leader_id %lu, last_included_index %lu, last_included_term %lu.",
                  request.term(), request.leader_id(), request.last_included_index(), request.last_included_term());

    std::string str_request;
    roo::ProtoBuf::marshalling_to_string(request, &str_request);

    peer.send_raft_RPC(tzrpc::ServiceID::RAFT_SERVICE, Raft::OpCode::kInstallSnapshot, str_request);
    return 0;
}


void RaftConsensus::main_thread_loop() {

    while (!main_thread_stop_) {

        {
            std::unique_lock<std::mutex> lock(consensus_mutex_);

            auto expire_tp = steady_clock::now() + std::chrono::milliseconds(100);
#if __cplusplus >= 201103L
            consensus_notify_.wait_until(lock, expire_tp);
#else
            consensus_notify_.wait_until(lock, expire_tp);
#endif
        }

        switch (context_->role()) {
            case Role::kFollower:
                if (election_timer_.timeout(option_.election_timeout_ms_)) {
                    roo::log_warning("Node %lu begin to request vote from Follower ...", context_->id());

                    std::lock_guard<std::mutex> lock(consensus_mutex_);

                    context_->become_candidate();
                    send_request_vote();
                    election_timer_.schedule();
                }
                break;

            case Role::kCandidate:
                if (election_timer_.timeout(option_.election_timeout_ms_)) {
                    roo::log_warning("Node %lu begin to request vote from Candidate ...", context_->id());

                    std::lock_guard<std::mutex> lock(consensus_mutex_);

                    context_->become_candidate();
                    send_request_vote();
                    election_timer_.schedule();
                }
                break;

            case Role::kLeader:
                if (heartbeat_timer_.timeout(option_.heartbeat_ms_)) {

                    std::lock_guard<std::mutex> lock(consensus_mutex_);

                    send_append_entries();
                    heartbeat_timer_.schedule();
                }
                break;

            default:
                std::string message = roo::va_format("Invalid role found: %d", static_cast<int32_t>(context_->role()));
                PANIC(message.c_str());
                break;
        }
    }
}

} // namespace sisyphus
