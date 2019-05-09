#ifndef __RAFT_OPTION_H__
#define __RAFT_OPTION_H__

#include <xtra_rhel.h>


// Raft相关的配置参数

namespace sisyphus {

struct Option {

    uint64_t id_;
    std::string members_str_;
    std::map<uint64_t, std::pair<std::string, uint16_t>> members_;
    std::string log_path_;

    // 心跳发送周期
    uint64_t heartbeat_tick_;

    // 选举超时
    // 如果在这个时间内都没收到Leader的HeartBeat或者RequestVote请求
    uint64_t election_timeout_tick_;


    Option() :
        id_(0),
        heartbeat_tick_(0),
        election_timeout_tick_(0) { }

    ~Option() = default;

    bool validate() const {

        // 1 + n
        if (members_str_.empty() || members_.size() < 2)
            return false;

        if (id_ == 0)
            return false;

        if (log_path_.empty())
            return false;

        if (heartbeat_tick_ == 0 || election_timeout_tick_ == 0 ||
            heartbeat_tick_ >= election_timeout_tick_)
            return false;

        return true;
    }

    std::string str() const {

        std::stringstream ss;
        ss << "Raft Configure:" << std::endl
            << "    id: " << id_ << std::endl
            << "    members: " << members_str_ << std::endl
            << "    log_path: " << log_path_ << std::endl
            << "    heartbeat_tick_: " << heartbeat_tick_ << std::endl
            << "    election_timeout_tick: " << election_timeout_tick_ << std::endl
        ;

        return ss.str();
    }
};

} // namespace sisyphus

#endif // __RAFT_OPTION_H__
