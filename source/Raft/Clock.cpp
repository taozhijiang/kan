
#include <concurrency/Timer.h>

// 如果直接处理时间的话可能会比较抓狂，所以所有Raft的操作都采用逻辑时钟
// 来控制，即每 100ms定时器将当前的时钟值增加 1
// 所有Raft超时的控制都使用该逻辑时钟来控制

#include <Captain.h>
#include <Raft/RaftConsensus.h>

#include <Raft/Clock.h>


namespace sisyphus {

volatile uint64_t Clock::current_ = 0;


void Clock::step(const boost::system::error_code& ec) {

    ++current_;

    // TODO, ugly
    Captain::instance().raft_consensus_ptr_->main_notify_.notify_all();
}


} // namespace sisyphus

