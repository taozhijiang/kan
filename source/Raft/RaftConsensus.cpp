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
    if(!setting_ptr) {
        roo::log_err("request setting failed.");
        return false;
    }

    setting_ptr->lookupValue("Raft.server_id", option_.id_);
    setting_ptr->lookupValue("Raft.storage_prefix", option_.log_path_);
    setting_ptr->lookupValue("Raft.heartbeat_tick", option_.heartbeat_tick_);
    setting_ptr->lookupValue("Raft.election_timeout_tick", option_.election_timeout_tick_);  


    // if not found, will throw exceptions
    const libconfig::Setting& peers = setting_ptr->lookup("Raft.cluster_peers");
    for(int i = 0; i < peers.getLength(); ++i) {
        
        uint64_t id;
        std::string addr;
        uint64_t port;
        const libconfig::Setting& peer = peers[i];

        peer.lookupValue("server_id", id);
        peer.lookupValue("addr", addr);
        peer.lookupValue("port", port);

        if(id == 0 || addr.empty() || port == 0) {
            roo::log_err("problem peer setting: %lu, %s, %lu", id, addr.c_str(), port);
            continue;
        }

        if(option_.members_.find(id) != option_.members_.end()) {
            roo::log_err("member already added before: %lu, %s, %lu", id, addr.c_str(), port);
            continue;
        }

        option_.members_[id] = std::make_pair(addr, port);
        option_.members_str_ += roo::StrUtil::to_string(id) + ">" + addr + ":" + roo::StrUtil::to_string(port) + ",";
    }

    if(!option_.validate()) {
        roo::log_err("validate raft option failed!");
        roo::log_err("current settings: %s", option_.str().c_str());
        return false;
    }


    // 初始化 peer_map_ 的主机列表
    for(auto iter = option_.members_.begin(); iter != option_.members_.end(); ++ iter) {
        auto endpoint = iter->second;
        auto peer = std::make_shared<Peer>(iter->first, endpoint.first, endpoint.second, 
                                           std::bind(&RaftConsensus::handle_rpc_callback, this,
                                           std::placeholders::_1, std::placeholders::_2,
                                           std::placeholders::_3, std::placeholders::_4));
        if(!peer) {
            roo::log_err("create peer member instance %lu failed.", iter->first);
            return false;
        }

        peer_map_[iter->first] = peer;
    }
    roo::log_warning("successful detected and initialized %lu peers!", peer_map_.size());


    // create context
    context_ = make_unique<Context>(option_.id_);
    if(!context_) {
        roo::log_err("create context failed.");
        return false;
    }

    
    // adjust log store path
    option_.log_path_ += "/instance_" + roo::StrUtil::to_string(option_.id_);
    if(!roo::FilesystemUtil::exists(option_.log_path_)) {
        ::mkdir(option_.log_path_.c_str(), 0755);
        if(!roo::FilesystemUtil::exists(option_.log_path_)) {
            roo::log_err("create path %s failed.", option_.log_path_.c_str());
            return false;
        }
    }

    log_meta_ = make_unique<LevelDBLog>(option_.log_path_ + "/log_meta");
    if(!log_meta_) {
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

    return true;
}


int RaftConsensus::handle_rpc_callback(RpcClientStatus status, uint16_t service_id, uint16_t opcode, const std::string& rsp) {
    
    if(status != RpcClientStatus::OK) {
        roo::log_err("rpc call failed with %d, service_id %u and opcode %u", 
                    static_cast<uint8_t>(status), service_id, opcode);
        return -1;
    }

    if(service_id != static_cast<uint16_t>(tzrpc::ServiceID::RAFT_SERVICE)) {
        roo::log_err("invalid service_id %u, expect %d",
                    service_id, tzrpc::ServiceID::RAFT_SERVICE);
        return -1;
    }
    
    // 解析异步响应报文，分发执行相应的消息处理
    if(opcode == static_cast<uint16_t>(OpCode::kRequestVote)) {
        Raft::RequestVoteOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("unmarshal response failed.");
            return -1;
        }
        return do_handle_request_vote_response(response);
    }
    else if(opcode == static_cast<uint16_t>(OpCode::kRequestVote)) {
        Raft::AppendEntriesOps::Response response;
        if (!roo::ProtoBuf::unmarshalling_from_string(rsp, &response)) {
            roo::log_err("unmarshal response failed.");
            return -1;
        }
        return do_handle_append_entries_response(response);
    }
    else if(opcode == static_cast<uint16_t>(OpCode::kRequestVote)) {
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

    response.set_term(request.term());
    response.set_vote_granted(true);

    return 0;
}

int RaftConsensus::do_handle_append_entries_request(
    const Raft::AppendEntriesOps::Request& request,
    Raft::AppendEntriesOps::Response& response) {

    return 0;
}

int RaftConsensus::do_handle_install_snapshot_request(
    const Raft::InstallSnapshotOps::Request& request,
    Raft::InstallSnapshotOps::Response& response) {

    return 0;
}




int RaftConsensus::do_handle_request_vote_response(const Raft::RequestVoteOps::Response& response) {

    // 如果已经发现其他的leader了
    if(response.term() > context_->term()) {
        roo::log_warning("Candidate request_vote but found higher term, revert to fellower");
        context_->become_follower(response.term(), response.peer_id());
        return 0;
    }

    // 
    if(context_->role() == Role::kCandidate) {
        if(response.vote_granted() == true) {
            
            uint64_t peer_id = response.peer_id();
            if(peer_map_.find(peer_id) == peer_map_.end()) {
                roo::log_err("unknown peer_id response: %lu", peer_id);
                return -1;
            }

            if(context_->quorum_granted_.find(peer_id) != context_->quorum_granted_.end()) {
                roo::log_err("duplicate vote response: %lu", peer_id);
                return -1;
            }

            context_->quorum_granted_.insert(peer_id);

            if(context_->quorum_granted_.size() > (option_.members_.size() + 1) / 2) {
                context_->become_leader();
                send_append_entries();
            }
        }
    }

    return 0;
}


int RaftConsensus::do_handle_append_entries_response(const Raft::AppendEntriesOps::Response& response) {

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
    if(!entry) {
        roo::log_warning("Get last entry failed, initial start??");
        request.set_last_log_index(0);
        request.set_last_log_term(0);
    } else {
        request.set_last_log_index(entry->index());
        request.set_last_log_term(entry->term());
    }

    std::string str_request;
    roo::ProtoBuf::marshalling_to_string(request, &str_request);

    for(auto iter = peer_map_.begin(); iter != peer_map_.end(); ++iter) {
        iter->second->send_rpc(tzrpc::ServiceID::RAFT_SERVICE, Raft::OpCode::kRequestVote, str_request);
    }

    return 0;
}


int RaftConsensus::send_append_entries() {
    return 0;
}


int RaftConsensus::send_install_snapshot() {
    return 0;
}


void RaftConsensus::main_thread_run() {

    while(!main_thread_stop_) {

        {
            std::unique_lock<std::mutex> lock(main_mutex_);
            main_notify_.wait(lock);
        }

        switch(context_->role()) {
            case Role::kFollower:
                if(context_->latest_oper_tick() + option_.election_timeout_tick_ < Clock::current()) {
                    context_->become_candidate();
                    send_request_vote();
                }
                break;

            case Role::kCandidate:
                break;

            case Role::kLeader:
                break;

            default:
                roo::log_err("Invalid role found: %d", static_cast<int32_t>(context_->role()));
                break;
        }
    }

}

} // namespace sisyphus
