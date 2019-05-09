#ifndef __RAFT_CLOCK_H__
#define __RAFT_CLOCK_H__

#include <xtra_rhel.h>

namespace sisyphus {

class Clock {

public:
    static void step(const boost::system::error_code& ec);

    static uint32_t tick_step() { return kTickStep; }
    static uint64_t current() { return current_; }

private:
    static volatile uint64_t current_;
    static const uint32_t kTickStep = 100;
};


// 简易用来进行心跳、选举等操作的定时器
class SimpleTimer {
public:
    explicit SimpleTimer() :
        enable_(false),
        start_tick_(0) {
    }

    bool timeout(uint64_t timeout) const {
        if (enable_)
            return start_tick_ + timeout < Clock::current();

        return false;
    }

    void schedule() {
        enable_ = true;
        start_tick_ = Clock::current();
    }

    void disable() {
        enable_ = false;
    }

private:
    bool enable_;
    uint64_t start_tick_;
};

} // namespace sisyphus


#endif // __RAFT_CLOCK_H__
