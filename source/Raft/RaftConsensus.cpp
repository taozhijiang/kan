#include <concurrency/Timer.h>
#include <scaffold/Setting.h>
#include <other/Log.h>
#include <string/StrUtil.h>
#include <other/FilesystemUtil.h>

#include <Protocol/Common.h>
#include <Raft/LevelDBLog.h>

#include <Raft/Clock.h>

#include <Captain.h>

#include "RaftConsensus.h"

namespace sisyphus {


bool RaftConsensus::init() {

    auto setting_ptr = Captain::instance().setting_ptr_->get_setting();
    if (!setting_ptr) {
        roo::log_err("request setting failed.");
        return false;
    }

    setting_ptr->lookupValue("Raft.bootstrap", option_.bootstrap_);
    setting_ptr->lookupValue("Raft.server_id", option_.id_);
    setting_ptr->lookupValue("Raft.storage_prefix", option_.log_path_);
    setting_ptr->lookupValue("Raft.heartbeat_tick", option_.heartbeat_tick_);
    setting_ptr->lookupValue("Raft.election_timeout_tick", option_.election_timeout_tick_);


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
            roo::log_err("problem peer setting: %lu, %s, %lu", id, addr.c_str(), port);
            continue;
        }

        if (option_.members_.find(id) != option_.members_.end()) {
            roo::log_err("member already added before: %lu, %s, %lu", id, addr.c_str(), port);
            continue;
        }

        option_.members_[id] = std::make_pair(addr, port);
        option_.members_str_ += roo::StrUtil::to_string(id) + ">" + addr + ":" + roo::StrUtil::to_string(port) + ",";
    }

    option_.withhold_votes_tick_ = option_.election_timeout_tick_;
    if (!option_.validate()) {
        roo::log_err("validate raft option failed!");
        roo::log_err("current settings: %s", option_.str().c_str());
        return false;
    }

    // 随机化选取超时定时器
    if (option_.election_timeout_tick_ > 2)
        option_.election_timeout_tick_ += ::random() % (option_.election_timeout_tick_ / 2);

    // 初始化 peer_map_ 的主机列表
    for (auto iter = option_.members_.begin(); iter != option_.members_.end(); ++iter) {
        auto endpoint = iter->second;
        auto peer = std::make_shared<Peer>(iter->first, endpoint.first, endpoint.second,
                                           std::bind(&RaftConsensus::handle_rpc_callback, this,
                                                     std::placeholders::_1, std::placeholders::_2,
                                                     std::placeholders::_3, std::placeholders::_4));
        if (!peer) {
            roo::log_err("create peer member instance %lu failed.", iter->first);
            return false;
        }

        peer_set_[iter->first] = peer;
    }
    roo::log_warning("successful detected and initialized %lu peers!", peer_set_.size());


    // adjust log store path
    option_.log_path_ += "/instance_" + roo::StrUtil::to_string(option_.id_);
    if (!roo::FilesystemUtil::exists(option_.log_path_)) {
        ::mkdir(option_.log_path_.c_str(), 0755);
        if (!roo::FilesystemUtil::exists(option_.log_path_)) {
            roo::log_err("create path %s failed.", option_.log_path_.c_str());
            return false;
        }
    }

    log_meta_ = make_unique<LevelDBLog>(option_.log_path_ + "/log_meta");
    if (!log_meta_) {
        roo::log_err("create log_meta_ failed.");
        return false;
    }


    // create context
    context_ = make_unique<Context>(option_.id_, log_meta_);
    if (!context_) {
        roo::log_err("create context failed.");
        return false;
    }

    // 获取meta数据
    LogIf::LogMeta meta;
    log_meta_->read_meta_data(&meta);
    if (meta.has_current_term())
        context_->set_term(meta.current_term());
    if (meta.has_voted_for())
        context_->set_voted_for(meta.voted_for());
    if (meta.has_commit_index())
        context_->set_commit_index(meta.commit_index());
    if (meta.has_apply_index())
        context_->set_voted_for(meta.voted_for());
    context_->update_meta();


    // bootstrap ???
    if (option_.bootstrap_) {
        roo::log_warning("bootstrap operation here ...");
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
        ::exit(EXIT_SUCCESS);
    }


    // 主工作线程
    main_thread_ = std::thread(std::bind(&RaftConsensus::main_thread_run, this));

    // 系统的时钟ticket
    if (!Captain::instance().timer_ptr_->add_timer(
            std::bind(&Clock::step, std::placeholders::_1),
            Clock::tick_step(), true)) {
        roo::log_err("create main tick timer failed.");
        return false;
    }

    // 启动选取定时器
    context_->become_follower(context_->term());
    election_timer_.schedule();

    return true;
}


int RaftConsensus::handle_rpc_callback(RpcClientStatus status, uint16_t service_id, uint16_t opcode, const std::string& rsp) {

    if (status != RpcClientStatus::OK) {
        roo::log_err("rpc call failed with %d, service_id %u and opcode %u",
                     static_cast<uint8_t>(status), service_id, opcode);
        return -1;
    }

    if (service_id != static_cast<uint16_t>(tzrpc::ServiceID::RAFT_SERVICE)) {
        roo::log_err("invalid service_id %u, expect %d",
                     service_id, tzrpc::ServiceID::RAFT_SERVICE);
        return -1;
    }

    // 解析异步响应报文，分发执行相应的消息处理
    if (opcode == static_cast<uint16_t>(OpCode::kRequestVote)) {
        Raft::RequestVoteOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("unmarshal response failed.");
            return -1;
        }
        return do_handle_request_vote_callback(response);
    } else if (opcode == static_cast<uint16_t>(OpCode::kAppendEntries)) {
        Raft::AppendEntriesOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("unmarshal response failed.");
            return -1;
        }
        return do_handle_append_entries_callback(response);
    } else if (opcode == static_cast<uint16_t>(OpCode::kInstallSnapshot)) {
        Raft::InstallSnapshotOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("unmarshal response failed.");
            return -1;
        }
        return do_handle_install_snapshot_callback(response);
    }

    roo::log_err("unexpected rpc call response with opcode %u", opcode);
    return -1;
}


//
// 响应请求
//
int RaftConsensus::do_handle_request_vote_request(const Raft::RequestVoteOps::Request& request,
                                                  Raft::RequestVoteOps::Response& response) {

    response.set_peer_id(context_->id());

    auto last_entry_term_index = log_meta_->last_term_and_index();
    bool log_is_ok = (request.last_log_term() > last_entry_term_index.first) ||
        (request.last_log_term() == last_entry_term_index.first && request.last_log_index() >= last_entry_term_index.second);

    if (withhold_votes_timer_.within(option_.withhold_votes_tick_)) {
        roo::log_warning("reject this request_vote, because we heard anthor leader with election_timeout ...");
        response.set_term(context_->term());
        response.set_vote_granted(false);
        response.set_log_ok(log_is_ok);
        return 0;
    }

    if (request.term() > context_->term()) {
        roo::log_warning("Request vote found large termer %lu compared with our %lu.", request.term(), context_->term());
        context_->become_follower(request.term());
    }

    if (request.term() < context_->term()) {
        roo::log_warning("Request vote found small termer %lu compared with our %lu.", request.term(), context_->term());
        response.set_term(context_->term());
        response.set_vote_granted(false);
        response.set_log_ok(log_is_ok);
        return 0;
    }


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

    roo::log_info("term %lu-%lu, voted_for %lu-%lu", context_->term(), request.term(),
                  context_->voted_for(), request.candidate_id());

    roo::log_info("request_vote response %s with log_ok: %d", voted_granted ? "true" : "false", log_is_ok);

    return 0;
}

int RaftConsensus::do_handle_append_entries_request(const Raft::AppendEntriesOps::Request& request,
                                                    Raft::AppendEntriesOps::Response& response) {

    response.set_peer_id(option_.id_);
    response.set_term(context_->term());
    response.set_success(false);
    response.set_last_log_index(log_meta_->last_index());

    if (request.term() < context_->term()) {
        roo::log_warning("recevied append_entries with term %lu, and our term is %lu", request.term(), context_->term());
        response.set_success(false);
        return 0;
    }

    if (request.term() > context_->term()) {
        roo::log_warning("received append_entries with larger term %lu, and our term is %lu", request.term(), context_->term());
        // bump up our term
        context_->set_term(request.term());
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
        roo::log_warning("our log is too old with %lu, reject leader's %lu.", log_meta_->last_index(), request.prev_log_index());
        response.set_success(false);
        return 0;
    }

    // 检查，保证前一条日志的term必须匹配，为了安全性考虑
    if (request.prev_log_index() >= log_meta_->start_index() &&
        log_meta_->entry(request.prev_log_index())->term() != request.prev_log_term()) {
        roo::log_err("previous log %lu with term not match %lu - %lu, so we reject this entry, and leader will override it later!",
                     request.prev_log_index(),
                     request.prev_log_term(), log_meta_->entry(request.prev_log_index())->term());
        response.set_success(false);
        return 0;
    }

    // 目前为止都好，将日志添加到本地，并更新响应索引
    response.set_success(true);
    // ....
    //

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

        roo::log_warning("we will append %lu log_entry from %lu", entries.size(), log_meta_->last_index());

        log_meta_->append(entries);
        break;
    }

    response.set_last_log_index(log_meta_->last_index());

    // Leader在设置该参数的时候，就确保了该值肯定不会超过该Peer的日志范围的
    if (context_->commit_index() < request.leader_commit()) {
        context_->set_commit_index(request.leader_commit());
        log_meta_->update_meta_commit_index(context_->commit_index());
    }

    return 0;
}

int RaftConsensus::do_handle_install_snapshot_request(const Raft::InstallSnapshotOps::Request& request,
                                                      Raft::InstallSnapshotOps::Response& response) {

    response.set_peer_id(option_.id_);

    return 0;
}




int RaftConsensus::do_handle_request_vote_callback(const Raft::RequestVoteOps::Response& response) {

    // 如果发现更大的term返回，则回退到follower
    if (response.term() > context_->term()) {

        roo::log_warning("Candidate request_vote but found higher term, revert to fellower");
        context_->become_follower(response.term());

        // update meta info
        LogIf::LogMeta meta;
        meta.set_current_term(response.term());
        meta.set_voted_for(response.peer_id());
        log_meta_->update_meta_data(meta);
        return 0;
    }

    // 候选者，检查选取结果
    if (context_->role() == Role::kCandidate) {

        if (response.vote_granted() == true) {

            uint64_t peer_id = response.peer_id();
            if (peer_set_.find(peer_id) == peer_set_.end()) {
                PANIC("responsed peer out of members.");
            }
            context_->add_quorum_granted(peer_id);

            // 选举成功
            if (context_->quorum_count() > (option_.members_.size() + 1) / 2) {

                roo::log_warning("node %lu has enough votes, and will become leader ...", context_->id());
                context_->become_leader();

                // 创建空的追加日志RPC
                LogIf::EntryPtr entry = std::make_shared<LogIf::Entry>();
                entry->set_term(context_->term());
                entry->set_type(Raft::EntryType::kNoop);
                auto idx = log_meta_->append({ entry });
                roo::log_debug("current log start_index %lu, last_index %lu", idx.first, idx.second);

                // 更新每个peer的数据
                for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter) {
                    iter->second->next_index_ = log_meta_->last_index() + 1;
                    iter->second->match_index_ = 0;
                }

                // 选举完成，切换成心跳定时器，关闭选取定时器
                election_timer_.disable();
                withhold_votes_timer_.disable();
                heartbeat_timer_.schedule();

                // 调度，根据Peer的next_index来发送
                send_append_entries();

                roo::log_warning("node %lu now became leader.", context_->id());

                return 0;
            }
        } else {
            // rejected by others
            roo::log_warning("RequestVote by %lu but rejected by peer %lu.",
                             context_->id(), response.peer_id());
            return 0;
        }
    }

    roo::log_warning("Role %s received request_vote, ignore it!", RoleStr(context_->role()).c_str());
    return 0;
}


int RaftConsensus::do_handle_append_entries_callback(const Raft::AppendEntriesOps::Response& response) {

    // 如果发现更大的term返回，则回退到follower
    if (response.term() > context_->term()) {

        roo::log_warning("Leader append_entries but found higher term, revert to fellower");
        context_->become_follower(response.term());

        // update meta info
        LogIf::LogMeta meta;
        meta.set_current_term(response.term());
        meta.set_voted_for(response.peer_id());
        log_meta_->update_meta_data(meta);
        return 0;
    }

    auto iter = peer_set_.find(response.peer_id());
    if (iter == peer_set_.end()) {
        roo::log_err("recv response outside of cluster with id %lu.", response.peer_id());
        return -1;
    }

    if (!response.has_last_log_index()) {
        PANIC("last_log_index is required!");
    }

    // 日志追加成功，更新next_index_
    if (response.success() == true) {
        roo::log_warning("append entries for peer %lu success.", response.peer_id());

        // 原始协议应该是prev_log_index + numEntries，但是这边没有原始调用的信息，所以
        // 就按照返回的last_log_index来设置了
        iter->second->match_index_ = response.last_log_index();
        iter->second->next_index_ = iter->second->match_index_  + 1;

        // TODO commit_index ...

        return 0;
    }

    if (iter->second->next_index_ > 1)
        --iter->second->next_index_;

    if (iter->second->next_index_ > response.last_log_index() + 1) {
        iter->second->next_index_ = response.last_log_index() + 1;
    }


    // 日志不匹配，减少next_index_然后重发
    roo::log_err("append entries for peer %lu failed, try from %lu", response.peer_id(), iter->second->next_index_);

    // schedule append_entries again
    send_append_entries(*iter->second);
    return 0;
}


int RaftConsensus::do_handle_install_snapshot_callback(const Raft::InstallSnapshotOps::Response& response) {

    return 0;
}



int RaftConsensus::send_request_vote() {

    // 构造请求报文
    Raft::RequestVoteOps::Request request;
    request.set_term(context_->term());
    request.set_candidate_id(context_->id());

    auto last_entry_term_index = log_meta_->last_term_and_index();
    request.set_last_log_term(last_entry_term_index.first);
    request.set_last_log_index(last_entry_term_index.second);

    std::string str_request;
    roo::ProtoBuf::marshalling_to_string(request, &str_request);

    for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter) {
        iter->second->send_rpc(tzrpc::ServiceID::RAFT_SERVICE, Raft::OpCode::kRequestVote, str_request);
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

    // 无论如何，send_append_entries都要发送，否则leader无法维持心跳
    // 发送的entry内容可以为空

    uint64_t prev_log_index = peer.next_index_ - 1;
    uint64_t prev_log_term = 0;

    if (prev_log_index >= log_meta_->start_index()) {
        LogIf::EntryPtr prev_log = log_meta_->entry(prev_log_index);
        if (!prev_log) {
            roo::log_err("current leader last_log_index %lu, peer %lu with next_id %lu",
                         log_meta_->last_index(), peer.id_, peer.next_index_);
            roo::log_err("Get prev_log entry index %lu for %lu failed.", prev_log_index, peer.id_);
            return -1;
        }

        prev_log_term = prev_log->term();
    }

    Raft::AppendEntriesOps::Request request;
    request.set_term(context_->term());
    request.set_leader_id(context_->id());
    request.set_prev_log_index(prev_log_index);
    request.set_prev_log_term(prev_log_term);

    // 可以为空，此时为纯粹的心跳
    // TODO: 限制每次发送的日志条目数
    if (!log_meta_->entries(peer.next_index_, entries)) {
        roo::log_err("Get entries for %lu failed, from %lu", peer.id_, prev_log_index);
        return -1;
    }

    google::protobuf::RepeatedPtrField<Raft::Entry>& request_entries = *request.mutable_entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        *request_entries.Add() = *(entries[i]);
    }

    request.set_leader_commit(std::min(context_->commit_index(), prev_log_index + entries.size()));
    roo::log_warning("send to peer %lu, with entries from %lu, size %u, commit_index %lu",
                     peer.id_, prev_log_index, request.entries().size(), request.leader_commit());

    std::string str_request;
    roo::ProtoBuf::marshalling_to_string(request, &str_request);
    peer.send_rpc(tzrpc::ServiceID::RAFT_SERVICE, Raft::OpCode::kAppendEntries, str_request);

    return 0;
}


int RaftConsensus::send_install_snapshot() {
    return -1;
}


void RaftConsensus::main_thread_run() {

    while (!main_thread_stop_) {

        {
            std::unique_lock<std::mutex> lock(main_mutex_);
            main_notify_.wait(lock);
        }

        switch (context_->role()) {
            case Role::kFollower:
                if (election_timer_.timeout(option_.election_timeout_tick_)) {
                    roo::log_warning("Node %lu begin to request vote from Follower ...", context_->id());
                    context_->become_candidate();
                    send_request_vote();
                    election_timer_.schedule();
                }
                break;

            case Role::kCandidate:
                if (election_timer_.timeout(option_.election_timeout_tick_)) {
                    roo::log_warning("Node %lu begin to request vote from Candidate ...", context_->id());
                    context_->become_candidate();
                    send_request_vote();
                    election_timer_.schedule();
                }
                break;

            case Role::kLeader:
                if (heartbeat_timer_.timeout(option_.heartbeat_tick_)) {
                    send_append_entries();
                }
                break;

            default:
                roo::log_err("Invalid role found: %d", static_cast<int32_t>(context_->role()));
                break;
        }
    }

}

} // namespace sisyphus
