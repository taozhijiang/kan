
#include <Raft/Context.h>


namespace sisyphus {


Context::Context(uint64_t id):
    id_(id),
    leader_id_(0),
    term_(0),
    voted_for_(0),
    quorum_granted_(),
    role_(Role::kFollower),
    latest_oper_tick_(0) {

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
    role_ = Role::kCandidate;
}



void Context::become_leader() {
    
    leader_id_ = id_;
    role_ = Role::kLeader;
}




} // namespace sisyphus
