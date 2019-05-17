/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <other/Log.h>

#include <Raft/Context.h>
#include <Raft/Clock.h>
#include <Raft/LogIf.h>


namespace sisyphus {


Context::Context(uint64_t id, std::unique_ptr<LogIf>& log_meta) :
    log_meta_(log_meta),
    id_(id),
    leader_id_(0),
    term_(0),
    voted_for_(0),
    quorum_granted_(),
    role_(Role::kFollower),
    commit_index_(0) {

}


void Context::become_follower(uint64_t term) {

    if (term_ < term) {
        roo::log_warning("Stepdown Node %lu from term %lu to %lu", id_, term_, term);
        term_ = term;
        leader_id_ = 0;
        voted_for_ = 0;

        update_meta();
    }

    role_ = Role::kFollower;
}


// 发起选取前的操作
void Context::become_candidate() {

    term_++;

    leader_id_ = 0;

    // 首先给自己投票
    voted_for_ = id_;
    quorum_granted_.clear();
    quorum_granted_.insert(id_);

    update_meta();
    role_ = Role::kCandidate;
}



void Context::become_leader() {

    leader_id_ = id_;
    role_ = Role::kLeader;
}


void Context::update_meta() {

    LogIf::LogMeta meta{};
    meta.set_current_term(term_);
    meta.set_voted_for(voted_for_);

    log_meta_->set_meta_data(meta);
}

std::string Context::str() const {

    std::stringstream ss;

    ss  << "   server_id: " << id_ << std::endl
        << "   leader_id: " << leader_id_ << std::endl
        << "   term: " << term_ << std::endl
        << "   voted_for: " << voted_for_ << std::endl
        << "   role: " << RoleStr(role_) << std::endl
        << "   commit_index:" << commit_index_ << std::endl
        << "   last_included_index:" << last_included_index_ << std::endl
        << "   last_included_term:" << last_included_term_ << std::endl;

    return ss.str();
}


std::ostream& operator<<(std::ostream& os, const Context& context) {
    os << context.str() << std::endl;
    return os;
}



} // namespace sisyphus
