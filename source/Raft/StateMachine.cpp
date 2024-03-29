/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <system/ConstructException.h>

#include <other/Log.h>

#include <Raft/RaftConsensus.h>
#include <Raft/LevelDBLog.h>
#include <Raft/LevelDBStore.h>
#include <Raft/StateMachine.h>

#include <message/ProtoBuf.h>
#include <Protocol/gen-cpp/Client.pb.h>

namespace kan {

StateMachine::StateMachine(RaftConsensus& raft_consensus,
                           std::unique_ptr<LogIf>& log_meta, std::unique_ptr<StoreIf>& kv_store) :
    raft_consensus_(raft_consensus),
    log_meta_(log_meta),
    kv_store_(kv_store),
    commit_index_(0),
    apply_index_(0),
    snapshot_progress_(SnapshotProgress::kDone),
    apply_mutex_(),
    apply_notify_(),
    apply_rsp_mutex_(),
    apply_rsp_(),
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
            auto expire_tp = std::chrono::system_clock::now() + std::chrono::seconds(5);

            std::unique_lock<std::mutex> lock(apply_mutex_);
#if __cplusplus >= 201103L
            apply_notify_.wait_until(lock, expire_tp);
#else
            apply_notify_.wait_until(lock, expire_tp);
#endif
        }

        LogIf::EntryPtr entry {};

again:

        if (snapshot_progress_ == SnapshotProgress::kBegin)
            snapshot_progress_ = SnapshotProgress::kProcessing;

        if (snapshot_progress_ == SnapshotProgress::kProcessing) {
            roo::log_info("Snapshot is processing, skip this loop ...");
            continue;
        }

        commit_index_ = log_meta_->meta_commit_index();
        if (commit_index_ < apply_index_) {
            PANIC("Corrupt commit_index %lu and apply_index %lu detected.",
                  commit_index_, apply_index_);
        }

        // 伪唤醒，无任何处理
        // 但是防止客户端死锁，还是通知一下
        if (commit_index_ == apply_index_) {
            raft_consensus_.client_notify();
            continue;
        }

        entry = log_meta_->entry(apply_index_ + 1);
        if (entry->type() == Raft::EntryType::kNoop) {
            roo::log_info("Skip NOOP type entry at term %lu, current processing apply_index %lu.",
                          entry->term(), apply_index_ + 1);
        } else if (entry->type() == Raft::EntryType::kNormal) {
            // 无论成功失败，都前进
            std::string content;
            if (do_apply(entry, content) == 0) {
                if (!content.empty()) {
                    std::lock_guard<std::mutex> lock(apply_rsp_mutex_);
                    apply_rsp_[apply_index_ + 1] = content;
                }
            }
            roo::log_info("Applied Normal type entry at term %lu, current processing apply_index %lu.",
                          entry->term(), apply_index_ + 1);
        } else {
            PANIC("Unhandled entry type %d found at term %lu, apply_index %lu.",
                  static_cast<int>(entry->type()), entry->term(), apply_index_);
        }

        // step forward apply_index
        ++apply_index_;
        log_meta_->set_meta_apply_index(apply_index_);

        // 如果提交队列有大量的日志需要执行，则不进行上述notify的等待
        if (commit_index_ > apply_index_) {
            if (apply_index_ % 20 == 0)
                raft_consensus_.client_notify();

            goto again;
        }


        raft_consensus_.client_notify();
    }

}


// do your specific business work here.
int StateMachine::do_apply(LogIf::EntryPtr entry, std::string& content_out) {

    std::string instruction = entry->data();

    kan::Client::StateMachineUpdateOps::Request request;
    if (!roo::ProtoBuf::unmarshalling_from_string(instruction, &request)) {
        roo::log_err("ProtoBuf unmarshal StateMachineWriteOps Request failed.");
        return -1;
    }

    int ret =  kv_store_->update_handle(request);

    if (ret == 0)
        content_out = "success at statemachine";
    else
        content_out = "failure at statemachine";

    return ret;
}


bool StateMachine::fetch_response_msg(uint64_t index, std::string& content) {

    std::lock_guard<std::mutex> lock(apply_rsp_mutex_);

    auto iter = apply_rsp_.find(index);
    if (iter == apply_rsp_.end())
        return false;

    content = iter->second;

    // 删除掉
    apply_rsp_.erase(index);

    // 剔除掉部分过于历史的响应

    //   uint64_t low_limit = apply_index_ < 5001 ? 1 : apply_index_ - 5000;
    //   auto low_bound = apply_rsp_.lower_bound(low_limit);
    //   apply_rsp_.erase(apply_rsp_.begin(), low_bound);

    return true;
}


bool StateMachine::create_snapshot(uint64_t& last_included_index, uint64_t& last_included_term) {

    snapshot_progress_ = SnapshotProgress::kBegin;
    notify_state_machine();
    while (snapshot_progress_ != SnapshotProgress::kProcessing) {
        ::usleep(10);
    }

    roo::log_warning("Begin to create snapshot ...");
    if (apply_index_ == 0) {
        roo::log_warning("StateMachine is initially state, give up.");
        snapshot_progress_ = SnapshotProgress::kDone;
        return true;
    }

    auto entry = log_meta_->entry(apply_index_);
    if (!entry) {
        roo::log_err("Get entry at %lu failed.", apply_index_);
        snapshot_progress_ = SnapshotProgress::kDone;
        return false;
    }

    bool result = kv_store_->create_snapshot(apply_index_, entry->term());
    if (result) {
        last_included_index = apply_index_;
        last_included_term  = entry->term();
    }

    snapshot_progress_ = SnapshotProgress::kDone;
    return result;
}

bool StateMachine::load_snapshot(std::string& content, uint64_t& last_included_index, uint64_t& last_included_term) {

    snapshot_progress_ = SnapshotProgress::kBegin;
    notify_state_machine();
    while (snapshot_progress_ != SnapshotProgress::kProcessing) {
        ::usleep(10);
    }

    roo::log_warning("Begin to load snapshot ...");
    bool result =  kv_store_->load_snapshot(content, last_included_index, last_included_term);

    snapshot_progress_ = SnapshotProgress::kDone;
    return result;
}

bool StateMachine::apply_snapshot(const Snapshot::SnapshotContent& snapshot) {

    snapshot_progress_ = SnapshotProgress::kBegin;
    notify_state_machine();
    while (snapshot_progress_ != SnapshotProgress::kProcessing) {
        ::usleep(10);
    }

    roo::log_warning("Begin to apply snapshot ...");
    bool result = kv_store_->apply_snapshot(snapshot);

    snapshot_progress_ = SnapshotProgress::kDone;
    return result;
}

} // namespace kan
