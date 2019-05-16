/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __RAFT_LEVELDB_LOG_H__
#define __RAFT_LEVELDB_LOG_H__

#include <xtra_rhel.h>
#include <mutex>
#include <leveldb/db.h>

#include <system/ConstructException.h>

#include <Raft/LogIf.h>

namespace sisyphus {

class LevelDBLog : public LogIf {

public:
    explicit LevelDBLog(const std::string& path);
    virtual ~LevelDBLog();

    // 返回start_index, last_index
    std::pair<uint64_t, uint64_t>
    append(const std::vector<EntryPtr>& newEntries)override;

    EntryPtr entry(uint64_t index) const override;
    bool     entries(uint64_t start, std::vector<EntryPtr>& entries, uint64_t limit) const override;
    std::pair<uint64_t, uint64_t>
    last_term_and_index() const override;

    uint64_t start_index() const override {
        return start_index_;
    }

    uint64_t last_index() const override {
        return last_index_;
    }

    void truncate_prefix(uint64_t start_index)override;
    void truncate_suffix(uint64_t last_index)override;


    // Raft运行状态元数据的持久化

    int meta_data(LogMeta* meta_data) const override;
    int set_meta_data(const LogMeta& meta) const override;

    uint64_t meta_commit_index() const override;
    uint64_t meta_apply_index() const override;
    int set_meta_commit_index(uint64_t commit_index) const override;
    int set_meta_apply_index(uint64_t apply_index) const override;

protected:

    // 最历史的日志索引，在日志压缩的时候会设置这个值
    uint64_t start_index_;
    uint64_t last_index_;   // 初始化为0，有效值从1开始

    mutable std::mutex  log_mutex_;
    const std::string   log_meta_path_;
    std::unique_ptr<leveldb::DB> log_meta_fp_;
};

} // namespace sisyphus

#endif // __RAFT_LEVELDB_LOG_H__
