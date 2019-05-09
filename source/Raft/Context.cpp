
#include <Raft/Context.h>
#include <Raft/Clock.h>

namespace sisyphus {


Context::Context(uint64_t id) :
    id_(id),
    leader_id_(0),
    term_(0),
    voted_for_(0),
    quorum_granted_(),
    role_(Role::kFollower),
    commit_index_(0),
    applied_index_(0) {

}


void Context::become_follower(uint64_t term, uint64_t leader) {

    term_ = term;
    leader_id_ = leader;
    role_ = Role::kFollower;
}


// 发起选取前的操作
void Context::become_candidate() {

    incr_term();

    leader_id_ = 0;

    // 给自己投票
    voted_for_ = id_;
    quorum_granted_.clear();
    quorum_granted_.insert(id_);
    role_ = Role::kCandidate;
}



void Context::become_leader() {

    leader_id_ = id_;
    role_ = Role::kLeader;
}


std::string Context::str() const {

    std::stringstream ss;

    ss
        << "    server_id: " << id_ << std::endl
        << "   leader_id: " << leader_id_ << std::endl
        << "   term: " << term_ << std::endl
        << "   voted_for: " << voted_for_ << std::endl
        << "   role: " << (role_ == Role::kLeader ? "leader" : role_ == Role::kCandidate ? "candidate" : "follower") << std::endl
        << "   commit_index:" << commit_index_ << std::endl
        << "   applied_index:" << applied_index_ << std::endl;

    return ss.str();
}



} // namespace sisyphus
