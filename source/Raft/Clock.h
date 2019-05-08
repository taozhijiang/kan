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


} // namespace sisyphus


#endif // __RAFT_CLOCK_H__
