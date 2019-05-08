
#include <concurrency/Timer.h>

// 如果直接处理时间的话可能会比较抓狂，所以所有Raft的操作都采用逻辑时钟
// 来控制，即每 100ms定时器将当前的时钟值增加 1
// 所有Raft超时的控制都使用该逻辑时钟来控制


namespace sisyphus {

class Clock {
public:


private:
    static const uint32_t kTickStep = 100;

    volatile current_tick_ ;
};



static volatile current_tick_ = 0;

int update_tick() {
    ++ current_tick_;
    return 0;
}

} // namespace sisyphus

