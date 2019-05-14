/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __RAFT_CLOCK_H__
#define __RAFT_CLOCK_H__

#include <xtra_rhel.h>


// 如果直接处理时间的话可能会比较抓狂，所以这边采用逻辑时钟的概念
// 来控制，即每 100ms定时器将当前的时钟值自增
// 内部涉及到的Raft的定时器都采用这个时钟来驱动


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

    // 是否在某个时间范围内，这边在投票优化的时候用到
    bool within(uint64_t timeout) const {
        if (enable_)
            return start_tick_ + timeout > Clock::current();

        return false;
    }

    // 启动定时器
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
