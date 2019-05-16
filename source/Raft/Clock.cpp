/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <concurrency/Timer.h>

#include <Captain.h>
#include <Raft/RaftConsensus.h>

#include <Raft/Clock.h>


namespace sisyphus {

volatile uint64_t Clock::current_ = 0;


void Clock::step(const boost::system::error_code& ec) {

    // 驱动RaftConsensus中的主循环进行状态和定时器的检查
    Captain::instance().raft_consensus_ptr_->concensus_notify_.notify_all();
}


} // namespace sisyphus

