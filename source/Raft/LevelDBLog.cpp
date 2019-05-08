#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <system/ConstructException.h>
#include <other/Log.h>
#include <string/Endian.h>

#include <Raft/LevelDBLog.h>


namespace sisyphus {

using roo::Endian;

static const char* META_CURRENT_TERM = "META_CURRENT_TERM";
static const char* META_VOTE_FOR     = "META_VOTE_FOR";
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
        throw roo::ConstructException("open leveldb failed.");
    }

    log_meta_fp_.reset(db);

    // skip meta, and get start_index_, last_index_
    std::unique_ptr<leveldb::Iterator> it(log_meta_fp_->NewIterator(leveldb::ReadOptions()));
    it->SeekToFirst();
    if (it->Valid()) {
        std::string key = it->key().ToString();
        if (key.find("META_") != std::string::npos) {
            start_index_ = Endian::uint64_from_net(key);
            roo::log_debug("seek found start_index_ %lu", start_index_);
        }
    }

    it->SeekToLast();
    while (it->Valid()) {

        std::string key = it->key().ToString();
        if (key.find("META_") == std::string::npos) {
            it->Prev();
            continue;
        }

        last_index_ = Endian::uint64_from_net(key);
        roo::log_debug("seek found last_index_ %lu", last_index_);
        break;
    }

    if (last_index_ != 0 && last_index_ > start_index_)
        throw roo::ConstructException("Invalid start_index_ and last_index_ detect.");
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
        roo::log_err("Append log failed.");
        last_index_ -= newEntries.size();
    }

    return{ start_index_, last_index_ };
}

LevelDBLog::EntryPtr LevelDBLog::get_entry(uint64_t index) const {

    std::string val;
    leveldb::Status status = log_meta_fp_->Get(leveldb::ReadOptions(), Endian::uint64_to_net(index), &val);
    if (!status.ok()) {
        roo::log_err("read %lu failed.", index);
        return EntryPtr();
    }

    EntryPtr entry = std::make_shared<Entry>();
    entry->ParseFromString(val);
    return entry;
}

LevelDBLog::EntryPtr LevelDBLog::get_last_entry() const {
    return get_entry(last_index_);
}


void LevelDBLog::truncate_prefix(uint64_t start_index) {

    std::lock_guard<std::mutex> lock(log_mutex_);

    if (start_index <= start_index_)
        return;

    leveldb::WriteBatch batch;
    for (; start_index_ <= start_index; ++start_index_) {
        batch.Delete(Endian::uint64_to_net(start_index_));
    }

    leveldb::Status status = log_meta_fp_->Write(leveldb::WriteOptions(), &batch);
    if (!status.ok()) {
        roo::log_err("truncate_prefix log failed.");
    }

}

void LevelDBLog::truncate_suffix(uint64_t last_index) {

    std::lock_guard<std::mutex> lock(log_mutex_);
    
    if (last_index >= last_index_)
        return;

    leveldb::WriteBatch batch;
    for (; last_index_ >= last_index; --last_index_) {
        batch.Delete(Endian::uint64_to_net(last_index_));
    }

    leveldb::Status status = log_meta_fp_->Write(leveldb::WriteOptions(), &batch);
    if (!status.ok()) {
        roo::log_err("truncate_suffix log failed.");
    }
}




int LevelDBLog::update_meta_data(const LogMeta& meta) const {

    leveldb::WriteBatch batch;
    batch.Put(META_CURRENT_TERM, Endian::uint64_to_net(meta.current_term()));
    batch.Put(META_VOTE_FOR,     Endian::uint64_to_net(meta.voted_for()));
    batch.Put(META_COMMIT_INDEX, Endian::uint64_to_net(meta.commit_index()));
    batch.Put(META_APPLY_INDEX,  Endian::uint64_to_net(meta.apply_index()));

    leveldb::Status status = log_meta_fp_->Write(leveldb::WriteOptions(), &batch);
    if (!status.ok()) {
        roo::log_err("update_meta_data write failed");
        return -1;
    }

    return 0;
}


int LevelDBLog::read_meta_data(LogMeta* meta_data) const {

    std::string val;
    leveldb::Status status;

    status = log_meta_fp_->Get(leveldb::ReadOptions(), META_CURRENT_TERM, &val);
    if (!status.ok()) {
        roo::log_err("read %s failed.", META_CURRENT_TERM);
        return -1;
    }
    meta_data->set_current_term(Endian::uint64_from_net(val));

    status = log_meta_fp_->Get(leveldb::ReadOptions(), META_VOTE_FOR, &val);
    if (!status.ok()) {
        roo::log_err("read %s failed.", META_VOTE_FOR);
        return -1;
    }
    meta_data->set_voted_for(Endian::uint64_from_net(val));

    status = log_meta_fp_->Get(leveldb::ReadOptions(), META_COMMIT_INDEX, &val);
    if (!status.ok()) {
        roo::log_err("read %s failed.", META_COMMIT_INDEX);
        return -1;
    }
    meta_data->set_commit_index(Endian::uint64_from_net(val));

    status = log_meta_fp_->Get(leveldb::ReadOptions(), META_APPLY_INDEX, &val);
    if (!status.ok()) {
        roo::log_err("read %s failed.", META_APPLY_INDEX);
        return -1;
    }
    meta_data->set_apply_index(Endian::uint64_from_net(val));

    return 0;
}




} // namespace sisyphus
