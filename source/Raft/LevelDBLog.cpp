/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <system/ConstructException.h>
#include <other/Log.h>
#include <string/StrUtil.h>
#include <string/Endian.h>

#include <Raft/LevelDBLog.h>


namespace sisyphus {

using roo::Endian;

static const char* META_CURRENT_TERM = "META_CURRENT_TERM";
static const char* META_VOTED_FOR    = "META_VOTED_FOR";
static const char* META_COMMIT_INDEX = "META_COMMIT_INDEX";
static const char* META_APPLY_INDEX  = "META_APPLY_INDEX";


LevelDBLog::LevelDBLog(const std::string& path) :
    start_index_(1),
    last_index_(0),
    log_mutex_(),
    log_meta_path_(path),
    log_meta_fp_() {

    leveldb::Options options;
    options.create_if_missing = true;
    // options.block_cache = leveldb::NewLRUCache(100 * 1048576);  // 100MB cache
    leveldb::DB* db;
    leveldb::Status status = leveldb::DB::Open(options, log_meta_path_, &db);

    if (!status.ok()) {
        roo::log_err("Open levelDB %s failed.", log_meta_path_.c_str());
        throw roo::ConstructException("Open levelDB failed.");
    }

    log_meta_fp_.reset(db);

    // skip meta, and get start_index_, last_index_
    std::unique_ptr<leveldb::Iterator> it(log_meta_fp_->NewIterator(leveldb::ReadOptions()));

#if 0
    it->SeekToFirst();
    while(it->Valid()) {
        std::string key = it->key().ToString();
        std::string val = it->value().ToString();
        std::cout << "log:" << key << "," << val << std::endl;
    }
#endif

    it->SeekToFirst();
    if (it->Valid()) {
        std::string key = it->key().ToString();
        if (key.find("META_") == std::string::npos) {
            start_index_ = Endian::uint64_from_net(key);
            roo::log_debug("Try seek and found start_index with %lu.", start_index_);
        }
    }

    it->SeekToLast();
    while (it->Valid()) {

        std::string key = it->key().ToString();
        // 元数据，这边跳过
        if (key.find("META_") != std::string::npos) {
            it->Prev();
            continue;
        }

        last_index_ = Endian::uint64_from_net(key);
        roo::log_debug("Try seek and found last_index %lu.", last_index_);
        break;
    }

    if (last_index_ != 0 && last_index_ < start_index_) {
        std::string message = roo::va_format("Invalid start_index %lu and last_index %lu", start_index_, last_index_);
        throw roo::ConstructException(message.c_str());
    }

    roo::log_warning("final start_index %lu, last_index %lu", start_index_, last_index_);
}

LevelDBLog::~LevelDBLog() {
    log_meta_fp_.reset();
}

std::pair<uint64_t, uint64_t>
LevelDBLog::append(const std::vector<EntryPtr>& newEntries) {

    std::lock_guard<std::mutex> lock(log_mutex_);

    leveldb::WriteBatch batch;
    for (size_t i = 0; i < newEntries.size(); ++i) {
        std::string buf;
        newEntries[i]->SerializeToString(&buf);
        last_index_++;
        batch.Put(Endian::uint64_to_net(last_index_), buf);
    }

    leveldb::Status status = log_meta_fp_->Write(leveldb::WriteOptions(), &batch);
    if (!status.ok()) {
        roo::log_err("Append log failed, affected entries size: %lu.", newEntries.size());
        last_index_ -= newEntries.size();
    }

    return{ start_index_, last_index_ };
}

LevelDBLog::EntryPtr LevelDBLog::entry(uint64_t index) const {

    std::string val;
    leveldb::Status status = log_meta_fp_->Get(leveldb::ReadOptions(), Endian::uint64_to_net(index), &val);
    if (!status.ok()) {
        roo::log_err("Read entry at index %lu failed.", index);
        return EntryPtr();
    }

    EntryPtr entry = std::make_shared<Entry>();
    entry->ParseFromString(val);
    return entry;
}

bool LevelDBLog::entries(uint64_t start, std::vector<EntryPtr>& entries, uint64_t limit) const {

    std::lock_guard<std::mutex> lock(log_mutex_);

    if (start < start_index_)
        start = start_index_;

    // 在peer和leader日志一致的时候，就会是这种情况
    if (start > last_index_) {
        return true;
    }

    uint64_t count = 0;
    for (; start <= last_index_; ++start) {
        std::string val;
        leveldb::Status status = log_meta_fp_->Get(leveldb::ReadOptions(), Endian::uint64_to_net(start), &val);
        if (!status.ok()) {
            roo::log_err("Read entries at index %lu failed.", start);
            return false;
        }

        EntryPtr entry = std::make_shared<Entry>();
        entry->ParseFromString(val);
        entries.emplace_back(entry);

        if (limit != 0 && ++count > limit)
            break;
    }

    return true;
}

std::pair<uint64_t, uint64_t>
LevelDBLog::last_term_and_index() const {

    std::lock_guard<std::mutex> lock(log_mutex_);

    if (last_index_ < start_index_) {
        return{ 0, last_index_ };
    }

    auto item = entry(last_index_);
    if (item)
        return{ item->term(), last_index_ };

    return{ 0, last_index_ };
}

// Delete the log entries before the given index.
// After this call, the log will contain no entries indexed less than start_index
void LevelDBLog::truncate_prefix(uint64_t start_index) {

    std::lock_guard<std::mutex> lock(log_mutex_);

    if (start_index <= start_index_)
        return;

    leveldb::WriteBatch batch;
    for (; start_index_ < start_index; ++start_index_) {
        batch.Delete(Endian::uint64_to_net(start_index_));
    }

    leveldb::Status status = log_meta_fp_->Write(leveldb::WriteOptions(), &batch);
    if (!status.ok()) {
        roo::log_err("TruncatePrefix log entries from index %lu failed.", start_index);
    }

    if (last_index_ < start_index_ - 1)
        last_index_ = start_index_ - 1;

    roo::log_info("TruncatePrefix at %lu finished, current start_index %lu, last_index %lu.",
                  start_index, start_index_, last_index_);
}

// Delete the log entries past the given index.
// After this call, the log will contain no entries indexed greater than last_index.
void LevelDBLog::truncate_suffix(uint64_t last_index) {

    std::lock_guard<std::mutex> lock(log_mutex_);

    if (last_index >= last_index_)
        return;

    leveldb::WriteBatch batch;
    for (; last_index_ > last_index; --last_index_) {
        batch.Delete(Endian::uint64_to_net(last_index_));
    }

    leveldb::Status status = log_meta_fp_->Write(leveldb::WriteOptions(), &batch);
    if (!status.ok()) {
        roo::log_err("TruncateSuffix log entries from last index %lu failed.", last_index);
    }

    if (last_index_ < start_index_ - 1)
        last_index_ = start_index_ - 1;

    roo::log_info("Truncatesuffix at %lu finished, current start_index %lu, last_index %lu.",
                  last_index, start_index_, last_index_);
}



int LevelDBLog::meta_data(LogMeta* meta_data) const {

    std::string val;
    leveldb::Status status;

    status = log_meta_fp_->Get(leveldb::ReadOptions(), META_CURRENT_TERM, &val);
    if (status.ok())
        meta_data->set_current_term(Endian::uint64_from_net(val));

    status = log_meta_fp_->Get(leveldb::ReadOptions(), META_VOTED_FOR, &val);
    if (status.ok())
        meta_data->set_voted_for(Endian::uint64_from_net(val));

    status = log_meta_fp_->Get(leveldb::ReadOptions(), META_COMMIT_INDEX, &val);
    if (status.ok())
        meta_data->set_commit_index(Endian::uint64_from_net(val));

    status = log_meta_fp_->Get(leveldb::ReadOptions(), META_APPLY_INDEX, &val);
    if (status.ok())
        meta_data->set_apply_index(Endian::uint64_from_net(val));

    return 0;
}


int LevelDBLog::set_meta_data(const LogMeta& meta) const {

    leveldb::WriteBatch batch;
    batch.Put(META_CURRENT_TERM,     Endian::uint64_to_net(meta.current_term()));
    batch.Put(META_VOTED_FOR,        Endian::uint64_to_net(meta.voted_for()));

    if (meta.has_commit_index())
        batch.Put(META_COMMIT_INDEX, Endian::uint64_to_net(meta.commit_index()));
    if (meta.has_apply_index())
        batch.Put(META_APPLY_INDEX,  Endian::uint64_to_net(meta.apply_index()));

    leveldb::Status status = log_meta_fp_->Write(leveldb::WriteOptions(), &batch);
    if (!status.ok()) {
        roo::log_err("Update Meta data failed.");
        roo::log_err("info: %s %lu, %s %lu, %s %lu, %s %lu.",
                     META_CURRENT_TERM, meta.current_term(), META_VOTED_FOR, meta.voted_for(),
                     META_COMMIT_INDEX, meta.commit_index(), META_APPLY_INDEX, meta.apply_index());
        return -1;
    }

    return 0;
}

uint64_t LevelDBLog::meta_commit_index() const {

    std::string val;
    leveldb::Status status = log_meta_fp_->Get(leveldb::ReadOptions(), META_COMMIT_INDEX, &val);
    if (status.ok())
        return Endian::uint64_from_net(val);

    return 0;
}


uint64_t LevelDBLog::meta_apply_index() const {

    std::string val;
    leveldb::Status status = log_meta_fp_->Get(leveldb::ReadOptions(), META_APPLY_INDEX, &val);
    if (status.ok())
        return Endian::uint64_from_net(val);

    return 0;
}

int LevelDBLog::set_meta_commit_index(uint64_t commit_index) const {
    leveldb::Status status = log_meta_fp_->Put(leveldb::WriteOptions(),
                                               META_COMMIT_INDEX, Endian::uint64_to_net(commit_index));
    if (!status.ok()) {
        roo::log_err("Update Meta set %s = %lu failed.", META_COMMIT_INDEX, commit_index);
        return -1;
    }
    return 0;
}

int LevelDBLog::set_meta_apply_index(uint64_t apply_index) const {
    leveldb::Status status = log_meta_fp_->Put(leveldb::WriteOptions(),
                                               META_APPLY_INDEX, Endian::uint64_to_net(apply_index));
    if (!status.ok()) {
        roo::log_err("Update Meta set %s = %lu failed.", META_APPLY_INDEX, apply_index);
        return -1;
    }
    return 0;
}


} // namespace sisyphus
