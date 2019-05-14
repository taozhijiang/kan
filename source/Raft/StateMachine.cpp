/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <system/ConstructException.h>

#include <other/Log.h>

#include <Raft/LevelDBLog.h>
#include <Raft/LevelDBStore.h>
#include <Raft/StateMachine.h>

#include <message/ProtoBuf.h>
#include <Protocol/gen-cpp/Client.pb.h>

namespace sisyphus {

StateMachine::StateMachine(std::unique_ptr<LogIf>& log_meta, std::unique_ptr<StoreIf>& kv_store) :
    log_meta_(log_meta),
    kv_store_(kv_store),
    commit_index_(0),
    apply_index_(0),
    lock_(),
    apply_notify_(),
    main_executor_stop_(false) {

    if (!log_meta_ || !kv_store)
        throw roo::ConstructException("invalid log_meta and kv_store provided.");
}

bool StateMachine::init() {

    commit_index_ = log_meta_->meta_commit_index();
    apply_index_ = log_meta_->meta_apply_index();

    if (commit_index_ < apply_index_) {
        roo::log_err("corrupt commit_index %lu and apply_index %lu detected.",
                     commit_index_, apply_index_);
        return false;
    }

    main_executor_ = std::thread(&StateMachine::state_machine_loop, this);
    return true;
}

void StateMachine::state_machine_loop() {

    while (!main_executor_stop_) {

        {
            std::unique_lock<std::mutex> lock(lock_);
            auto expire_tp = std::chrono::system_clock::now() + std::chrono::seconds(5);

#if __cplusplus >= 201103L
            apply_notify_.wait_until(lock, expire_tp);
#else
            apply_notify_.wait_until(lock, expire_tp);
#endif
        }

        commit_index_ = log_meta_->meta_commit_index();
        if (commit_index_ < apply_index_) {
            PANIC("corrupt commit_index %lu and apply_index %lu detected.",
                  commit_index_, apply_index_);
        }

        // 伪唤醒，无任何处理
        if (commit_index_ == apply_index_)
            continue;

        LogIf::EntryPtr entry = log_meta_->entry(apply_index_ + 1);
        if (entry->type() == Raft::EntryType::kNoop) {
            roo::log_info("Skip NOOP entry at term %lu, apply_index %lu.", entry->term(), apply_index_);
        } else if (entry->type() == Raft::EntryType::kNormal) {
            // 无论成功失败，都前进
            do_apply(entry);
            roo::log_info("Applied Normal entry at term %lu, apply_index %lu", entry->term(), apply_index_);
        } else {
            PANIC("Unhandled entry type %d, at term %lu, apply_index %lu",
                  static_cast<int>(entry->type()), entry->term(), apply_index_);
        }

        // step forward apply_index
        ++apply_index_;
        log_meta_->set_meta_apply_index(apply_index_);
    }

}


// do your specific business work here.
int StateMachine::do_apply(LogIf::EntryPtr entry) {

    std::string instruction = entry->data();

    sisyphus::Client::StateMachineUpdateOps::Request request;
    if (!roo::ProtoBuf::unmarshalling_from_string(instruction, &request)) {
        roo::log_err("unmarshal StateMachineWriteOps Request failed.");
        return -1;
    }

    return kv_store_->update_handle(request);
}


} // namespace
