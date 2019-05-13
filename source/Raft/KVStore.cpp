#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include <system/ConstructException.h>
#include <other/Log.h>

#include <Raft/KVStore.h>

namespace sisyphus {


KVStore::KVStore(const std::string& path) :
    kv_path_(path),
    kv_fp_() {

    leveldb::Options options;
    options.create_if_missing = true;
    // options.block_cache = leveldb::NewLRUCache(100 * 1048576);  // 100MB cache
    leveldb::DB* db;
    leveldb::Status status = leveldb::DB::Open(options, kv_path_, &db);

    if (!status.ok()) {
        roo::log_err("Open levelDB %s failed.", kv_path_.c_str());
        throw roo::ConstructException("open leveldb failed.");
    }

    kv_fp_.reset(db);
}

KVStore::~KVStore() {
    kv_fp_.reset();
}


int KVStore::get(const std::string& key, std::string& val) const {
    leveldb::Status status = kv_fp_->Get(leveldb::ReadOptions(), key, &val);
    if (status.ok()) return 0;

    return -1;
}


int KVStore::set(const std::string& key, const std::string& val) const {

    leveldb::Status status = kv_fp_->Put(leveldb::WriteOptions(), key, val);
    if (status.ok()) return 0;

    return -1;
}


} // namespace sisyphus
