#include <other/Log.h>

#include <Raft/LogIf.h>

#include <Raft/StateMachine.h>

namespace sisyphus {

StateMachine::StateMachine(std::unique_ptr<LogIf>& log_meta):
    log_meta_(log_meta),
    commit_index_(0),
    apply_index_(0),
    lock_(),
    apply_notify_(),
    main_executor_stop_(false) {
    }

bool StateMachine::init() {

    commit_index_ = log_meta_->read_meta_commit_index();
    apply_index_ = log_meta_->read_meta_apply_index();

    if(commit_index_ < apply_index_) {
        roo::log_err("corrupt commit_index and apply_index %lu, %lu", commit_index_, apply_index_);
        return false;
    }

    main_executor_ = std::thread(&StateMachine::state_machine_run, this);
    return true;
}

void StateMachine::state_machine_run() {

    while(!main_executor_stop_) {

        {   
            std::unique_lock<std::mutex> lock(lock_);
            auto expire_tp = std::chrono::system_clock::now() + std::chrono::seconds(5);
        
#if __cplusplus >= 201103L
            apply_notify_.wait_until(lock, expire_tp);
#else
            apply_notify_.wait_until(lock, expire_tp);
#endif
        }
 
        commit_index_ = log_meta_->read_meta_commit_index();
        if(commit_index_ < apply_index_) {
            PANIC("corrupt commit_index and apply_index %lu, %lu", commit_index_, apply_index_);
        }

        if(commit_index_ == apply_index_) 
            continue;

        LogIf::EntryPtr entry = log_meta_->entry(apply_index_ + 1);

        if(entry->type() == Raft::EntryType::kNoop) {
            roo::log_info("skip apply Noop entry, at term %lu, index %lu", entry->term(), entry->index());
        } else if(entry->type() == Raft::EntryType::kNormal) {
            // 无论成功失败，都前进
            do_apply(entry);
            roo::log_info("applied entry at term %lu, index %lu", entry->term(), entry->index());
        } else {
            PANIC("unhandled entry with type %d, at term %lu, index %lu", 
            static_cast<int>(entry->type()), entry->term(), entry->index());
        }

        apply_index_ = entry->index();
        log_meta_->update_meta_apply_index(apply_index_);
    }

}


// do your business here.
int StateMachine::do_apply(LogIf::EntryPtr entry) {
    roo::log_warning("handle entry @%lu:%lu", entry->term(), entry->index());
    return 0;
}


} // namespace
