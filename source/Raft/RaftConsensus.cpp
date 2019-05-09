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


    // create context
    context_ = make_unique<Context>(option_.id_);
    if (!context_) {
        roo::log_err("create context failed.");
        return false;
    }


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
        return do_handle_request_vote_response(response);
    } else if (opcode == static_cast<uint16_t>(OpCode::kAppendEntries)) {
        Raft::AppendEntriesOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("unmarshal response failed.");
            return -1;
        }
        return do_handle_append_entries_response(response);
    } else if (opcode == static_cast<uint16_t>(OpCode::kInstallSnapshot)) {
        Raft::InstallSnapshotOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("unmarshal response failed.");
            return -1;
        }
        return do_handle_install_snapshot_response(response);
    }

    roo::log_err("unhandle response with opcode %u", opcode);
    return -1;
}


//
// 响应请求
//
int RaftConsensus::do_handle_request_vote_request(
    const Raft::RequestVoteOps::Request& request,
    Raft::RequestVoteOps::Response& response) {

    response.set_term(context_->term());
    response.set_peer_id(context_->id());

    if (request.term() > context_->term()) {
        roo::log_warning("Request vote found large term %lu compared with our %lu.", request.term(), context_->term());
        context_->become_follower(response.term(), response.peer_id());
    }

    if (request.term() < context_->term()) {
        roo::log_warning("Request vote found small term %lu compared with our %lu.", request.term(), context_->term());
        response.set_vote_granted(false);
        return 0;
    }

    uint64_t self_last_log_term = 0;
    uint64_t self_last_log_index = 0;
    LogIf::EntryPtr entry = log_meta_->get_last_entry();
    if (entry) {
        self_last_log_term = entry->term();
        self_last_log_index = entry->index();
    }

    if (request.last_log_term() < self_last_log_term ||
        (request.last_log_term() == self_last_log_term && request.last_log_index() < self_last_log_index)) {
        roo::log_warning("found small log term or index.");
        response.set_vote_granted(false);
        return 0;
    }

    response.set_peer_id(option_.id_);
    response.set_term(request.term());
    response.set_vote_granted(true);

    return 0;
}

int RaftConsensus::do_handle_append_entries_request(
    const Raft::AppendEntriesOps::Request& request,
    Raft::AppendEntriesOps::Response& response) {

    response.set_peer_id(option_.id_);

    return 0;
}

int RaftConsensus::do_handle_install_snapshot_request(
    const Raft::InstallSnapshotOps::Request& request,
    Raft::InstallSnapshotOps::Response& response) {

    response.set_peer_id(option_.id_);

    return 0;
}




int RaftConsensus::do_handle_request_vote_response(const Raft::RequestVoteOps::Response& response) {

    // 如果发现更大的term返回，则回退到follower
    if (response.term() > context_->term()) {

        roo::log_warning("Candidate request_vote but found higher term, revert to fellower");
        context_->become_follower(response.term(), response.peer_id());

        // update meta info
        LogIf::LogMeta meta;
        meta.set_current_term(response.term());
        meta.set_voted_for(response.peer_id());
        log_meta_->update_meta_data(meta);
        return 0;
    }

    //
    if (context_->role() == Role::kCandidate) {

        if (response.vote_granted() == true) {

            uint64_t peer_id = response.peer_id();
            if (peer_set_.find(peer_id) == peer_set_.end()) {
                roo::log_err("unknown peer_id response: %lu", peer_id);
                return -1;
            }

            if (context_->quorum_granted_.find(peer_id) != context_->quorum_granted_.end()) {
                roo::log_err("duplicate vote response: %lu", peer_id);
                return -1;
            }

            context_->quorum_granted_.insert(peer_id);

            if (context_->quorum_granted_.size() > (option_.members_.size() + 1) / 2) {

                roo::log_warning("enough votes, I will become leader ...");
                context_->become_leader();

                // 创建空的追加日志RPC
                LogIf::EntryPtr entry = std::make_shared<LogIf::Entry>();
                entry->set_term(context_->term());
                entry->set_type(Raft::EntryType::kNoop);
                log_meta_->append({ entry });


                // 更新每个peer的数据
                for (auto iter = peer_set_.begin(); iter != peer_set_.end(); ++iter) {
                    iter->second->next_index_ = log_meta_->last_index() + 1;
                    iter->second->match_index_ = 0;
                }


                // 调度，根据Peer的next_index来发送
                send_append_entries();

                // 选举完成，切换成心跳定时器，关闭选取定时器
                election_timer_.disable();
                heartbeat_timer_.schedule();

            }
        } else { // rejected by others
            roo::log_warning("RequestVote by %lu but rejected by %lu, may have not enought log",
                             context_->id(), response.peer_id());
            // become follower
            context_->become_follower(response.term(), response.peer_id());

            // update meta info
            LogIf::LogMeta meta;
            meta.set_current_term(response.term());
            meta.set_voted_for(response.peer_id());
            log_meta_->update_meta_data(meta);
            return 0;
        }
    }

    roo::log_warning("Role %s received request_vote, ignore it!", RoleStr(context_->role()).c_str());
    return 0;
}


int RaftConsensus::do_handle_append_entries_response(const Raft::AppendEntriesOps::Response& response) {

    // 如果发现更大的term返回，则回退到follower
    if (response.term() > context_->term()) {

        roo::log_warning("Leader append_entries but found higher term, revert to fellower");
        context_->become_follower(response.term(), response.peer_id());

        // update meta info
        LogIf::LogMeta meta;
        meta.set_current_term(response.term());
        meta.set_voted_for(response.peer_id());
        log_meta_->update_meta_data(meta);
        return 0;
    }

    auto iter = peer_set_.find(response.peer_id());
    if (iter == peer_set_.end()) {
        roo::log_err("recv response outside of cluster with %lu.", response.peer_id());
        return -1;
    }

    // 日志追加成功，更新next_index_
    if (response.success() == true) {
        iter->second->match_index_ = response.last_log_index();
        iter->second->next_index_ = response.last_log_index() + 1;

        // TODO commit_index ...

        return 0;
    }

    // 日志不匹配，减少next_index_然后重发
    iter->second->next_index_ = response.last_log_index() + 1;
    iter->second->match_index_ = std::min(iter->second->match_index_, response.last_log_index());
    send_append_entries(*iter->second);
    return 0;
}


int RaftConsensus::do_handle_install_snapshot_response(const Raft::InstallSnapshotOps::Response& response) {

    return 0;
}



int RaftConsensus::send_request_vote() {

    // 构造请求报文
    Raft::RequestVoteOps::Request request;
    request.set_term(context_->term());
    request.set_candidate_id(context_->id());

    LogIf::EntryPtr entry = log_meta_->get_last_entry();
    if (!entry) {
        roo::log_warning("Get last entry failed, initial start??");
        request.set_last_log_index(0);
        request.set_last_log_term(0);
    } else {
        request.set_last_log_index(entry->index());
        request.set_last_log_term(entry->term());
    }

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

    uint64_t prev_log_index = peer.next_index_ - 1;
    if (prev_log_index > log_meta_->last_index()) {
        roo::log_warning("Peer %lu already advanced next_index: %lu", peer.id_, peer.next_index_);
        return -1;
    }

    LogIf::EntryPtr prev_log = log_meta_->get_entry(prev_log_index);
    if (!prev_log) {
        roo::log_err("Get prev_log entry as %lu failed.", prev_log_index);
        return -1;
    }

    // 可以为空，此时为纯粹的心跳
    // TODO: 限制每次发送的日志条目数
    if (!log_meta_->get_entries(prev_log_index, entries)) {
        roo::log_err("Get entries for %lu failed, from %lu", peer.id_, prev_log_index);
        return -1;
    }


    Raft::AppendEntriesOps::Request request;
    request.set_term(context_->term());
    request.set_leader_id(context_->id());
    request.set_prev_log_index(prev_log_index);
    request.set_prev_log_term(prev_log->term());

    google::protobuf::RepeatedPtrField<Raft::Entry>& request_entries = *request.mutable_entries();
    for (size_t i = 0; i < entries.size(); ++i) {
        *request_entries.Add() = *(entries[i]);
    }
    request.set_leader_commit(std::min(context_->commit_index_, prev_log_index + entries.size()));
    roo::log_warning("send to peer %lu, with entries %u, commit_index %lu",
                     peer.id_, request_entries.size(), request.leader_commit());

    std::string str_request;
    roo::ProtoBuf::marshalling_to_string(request, &str_request);
    peer.send_rpc(tzrpc::ServiceID::RAFT_SERVICE, Raft::OpCode::kAppendEntries, str_request);

    return 0;
}


int RaftConsensus::send_install_snapshot() {
    return 0;
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
                    //                   send_append_entries();
                }
                break;

            default:
                roo::log_err("Invalid role found: %d", static_cast<int32_t>(context_->role()));
                break;
        }
    }

}

} // namespace sisyphus
