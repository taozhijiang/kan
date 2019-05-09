#ifndef __RAFT_CONTEXT_H__
#define __RAFT_CONTEXT_H__

#include <xtra_rhel.h>

namespace sisyphus {

enum class Role : uint8_t {
    kFollower = 1,
    kCandidate = 2,
    kLeader = 3,
};

static inline std::string RoleStr(enum Role role) {
    return (role == Role::kLeader ? "Leader" : role == Role::kFollower ? "Follower" : "Candidate");
}

class Context {

    friend class RaftConsensus;

public:
    explicit Context(uint64_t id);
    ~Context() = default;

public:

    uint64_t term() const { return term_; }
    uint64_t voted_for() const { return voted_for_; }
    uint64_t id() const { return id_; }
    uint64_t quorum_count() const { return quorum_granted_.size(); }
    Role role() const { return role_; }

    void incr_term() { ++term_; }

    // 角色切换
    void become_follower(uint64_t term, uint64_t leader);
    void become_candidate();
    void become_leader();

    std::string str() const;

private:

    const uint64_t id_;
    uint64_t leader_id_;

    uint64_t term_;
    uint64_t voted_for_;

    // 记录获得选票的peer
    std::set<uint64_t> quorum_granted_;

    enum Role role_;

    uint64_t commit_index_;
    uint64_t applied_index_;
};

} // namespace sisyphus

#endif // __RAFT_CONTEXT_H__

