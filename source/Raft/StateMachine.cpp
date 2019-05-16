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
    snapshot_progress_(SnapshotProgress::kDone),
    apply_mutex_(),
    apply_notify_(),
    main_executor_stop_(false) {

    if (!log_meta_ || !kv_store)
        throw roo::ConstructException("Construct StateMachine failed, invalid log_meta and kv_store provided.");
}

bool StateMachine::init() {

    commit_index_ = log_meta_->meta_commit_index();
    apply_index_ = log_meta_->meta_apply_index();

    if (commit_index_ < apply_index_) {
        roo::log_err("Corrupt commit_index %lu and apply_index %lu detected.",
                     commit_index_, apply_index_);
        return false;
    }

    main_executor_ = std::thread(&StateMachine::state_machine_loop, this);
    roo::log_warning("Init StateMachine successfully.");
    return true;
}

void StateMachine::state_machine_loop() {

    while (!main_executor_stop_) {

        {
            std::unique_lock<std::mutex> lock(apply_mutex_);
            auto expire_tp = std::chrono::system_clock::now() + std::chrono::seconds(5);

#if __cplusplus >= 201103L
            apply_notify_.wait_until(lock, expire_tp);
#else
            apply_notify_.wait_until(lock, expire_tp);
#endif
        }

        if(snapshot_progress_ == SnapshotProgress::kBegin) 
            snapshot_progress_ = SnapshotProgress::kProcessing;
        
        if(snapshot_progress_ == SnapshotProgress::kProcessing) {
            roo::log_info("Snapshot is processing ...");
            continue;
        }

        commit_index_ = log_meta_->meta_commit_index();
        if (commit_index_ < apply_index_) {
            PANIC("Corrupt commit_index %lu and apply_index %lu detected.",
                  commit_index_, apply_index_);
        }

        // 伪唤醒，无任何处理
        if (commit_index_ == apply_index_)
            continue;

        LogIf::EntryPtr entry = log_meta_->entry(apply_index_ + 1);
        if (entry->type() == Raft::EntryType::kNoop) {
            roo::log_info("Skip NOOP type entry at term %lu, apply_index %lu.", entry->term(), apply_index_);
        } else if (entry->type() == Raft::EntryType::kNormal) {
            // 无论成功失败，都前进
            do_apply(entry);
            roo::log_info("Applied Normal type entry at term %lu, apply_index %lu.", entry->term(), apply_index_);
        } else {
            PANIC("Unhandled entry type %d found at term %lu, apply_index %lu.",
                  static_cast<int>(entry->type()), entry->term(), apply_index_);
        }

        // step forward apply_index
        ++ apply_index_;
        log_meta_->set_meta_apply_index(apply_index_);
    }

}


// do your specific business work here.
int StateMachine::do_apply(LogIf::EntryPtr entry) {

    std::string instruction = entry->data();

    sisyphus::Client::StateMachineUpdateOps::Request request;
    if (!roo::ProtoBuf::unmarshalling_from_string(instruction, &request)) {
        roo::log_err("ProtoBuf unmarshal StateMachineWriteOps Request failed.");
        return -1;
    }

    return kv_store_->update_handle(request);
}


bool StateMachine::create_snapshot() {
    
    snapshot_progress_ = SnapshotProgress::kBegin;
    while(snapshot_progress_ != SnapshotProgress::kProcessing) {
        ::usleep(100);
    }

    roo::log_warning("Begin to create snapshot ...");
    if(apply_index_ == 0) {
        roo::log_warning("StateMachine is initially state, give up.");
        snapshot_progress_ = SnapshotProgress::kDone;
        return true;
    }

    auto entry = log_meta_->entry(apply_index_);
    if(!entry) {
        roo::log_err("Get entry at %lu failed.", apply_index_);
        snapshot_progress_ = SnapshotProgress::kDone;
        return false;
    }

    return kv_store_->create_snapshot(apply_index_, entry->term());
}

bool StateMachine::load_snapshot() {
    
    snapshot_progress_ = SnapshotProgress::kBegin;
    while(snapshot_progress_ != SnapshotProgress::kProcessing) {
        ::usleep(100);
    }

    roo::log_warning("Begin to load snapshot ...");
    uint64_t last_included_index = 0;
    uint64_t last_included_term  = 0;
    return kv_store_->load_snapshot(last_included_index, last_included_term);
}

} // namespace
