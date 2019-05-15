/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __RAFT_CONTEXT_H__
#define __RAFT_CONTEXT_H__

#include <xtra_rhel.h>

namespace sisyphus {

enum class Role : uint8_t {
    kFollower   = 1,
    kCandidate  = 2,
    kLeader     = 3,
};

static inline std::string RoleStr(enum Role role) {
    return (role == Role::kLeader ? "LEADER" : role == Role::kFollower ? "FOLLOWER" : "CANDIDATE");
}


// 需要更新访问meta数据
class LogIf;

class Context {

    //friend class RaftConsensus;

public:
    explicit Context(uint64_t id, std::unique_ptr<LogIf>& log_meta);
    ~Context() = default;

public:

    uint64_t id() const { return id_; }

    uint64_t leader_id() const { return leader_id_; }
    uint64_t term() const { return term_; }
    uint64_t voted_for() const { return voted_for_; }
    uint64_t quorum_count() const { return quorum_granted_.size(); }
    uint64_t commit_index() const { return commit_index_; }
    Role role() const { return role_; }

    void set_leader_id(uint64_t leader_id) { leader_id_ = leader_id; }
    void set_term(uint64_t term) { term_ = term; }
    void set_voted_for(uint64_t voted_for) { voted_for_ = voted_for; }
    void add_quorum_granted(uint64_t peer_id) { quorum_granted_.insert(peer_id); }
    void set_commit_index(uint64_t commit_index) { commit_index_ = commit_index; }

    // 集群中节点角色切换
    // same as stepdown
    void become_follower(uint64_t term);
    void become_candidate();
    void become_leader();

    void update_meta();

    std::string str() const;

private:

    std::unique_ptr<LogIf>& log_meta_;

    const uint64_t id_;
    uint64_t leader_id_;

    uint64_t term_;
    uint64_t voted_for_;

    // 记录获得选票的peer
    std::set<uint64_t> quorum_granted_;

    enum Role role_;

    uint64_t commit_index_;


    friend std::ostream& operator<<(std::ostream& os, const Context& context);
};

} // namespace sisyphus

#endif // __RAFT_CONTEXT_H__

