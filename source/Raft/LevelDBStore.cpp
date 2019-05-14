/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <leveldb/comparator.h>

#include <other/Log.h>
#include <message/ProtoBuf.h>

#include <Raft/LevelDBStore.h>


namespace sisyphus {


LevelDBStore::LevelDBStore(const std::string& path) :
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

LevelDBStore::~LevelDBStore() {
    kv_fp_.reset();
}


int LevelDBStore::select_handle(const Client::StateMachineSelectOps::Request& request,
                                Client::StateMachineSelectOps::Response& response) const {

    int result = 0;

    do {
        if (request.has_read()) {

            std::string content;
            if (get(request.read().key(), content) == 0) {
                response.mutable_read()->set_content(content);
            } else {
                roo::log_err("read %s failed.", request.read().key().c_str());
                result = -1;
            }
            break;

        } else if (request.has_range()) {

            std::vector<std::string> range_store;
            std::string start_key   = request.range().start_key();
            std::string end_key     = request.range().end_key();
            uint64_t limit          = request.range().limit();
            result = range(start_key, end_key, limit, range_store);
            if (result != 0) {
                roo::log_err("range for %s, %s, %lu failed.", start_key.c_str(), end_key.c_str(), limit);
                break;
            }

            *response.mutable_range()->mutable_contents()
                = { range_store.begin(), range_store.end() };
            break;

        } else if (request.has_search()) {

            std::vector<std::string> search_store;
            std::string search_key = request.search().search_key();
            uint64_t limit = request.search().limit();
            result = search(search_key, limit, search_store);
            if (result != 0) {
                roo::log_err("search for %s, %lu failed.", search_key.c_str(), limit);
                break;
            }

            *response.mutable_search()->mutable_contents()
                = { search_store.begin(), search_store.end() };
            break;

        }

        roo::log_err("Unknown operations for StateMachineSelectOps");
        result = -1;

    } while (0);

    if (result == 0) {
        response.set_code(0);
        response.set_msg("OK");
    } else {
        response.set_code(result);
        response.set_msg("FAIL");
    }

    return result;
}


int LevelDBStore::update_handle(const Client::StateMachineUpdateOps::Request& request) const {


    if (request.has_write()) {


        if (set(request.write().key(), request.write().content()) == 0)
            return 0;

        roo::log_err("write %s with %s failed.", request.write().key().c_str(), request.write().content().c_str());
        return -1;


    }

    if (request.has_remove()) {

        if (del(request.remove().key()) == 0)
            return 0;

        roo::log_err("del %s failed.", request.remove().key().c_str());
        return -1;

    }


    roo::log_err("Unknown operations for StateMachineUpdateOps");
    return -1;
}


int LevelDBStore::get(const std::string& key, std::string& val) const {

    leveldb::Status status = kv_fp_->Get(leveldb::ReadOptions(), key, &val);
    if (status.ok()) return 0;

    return -1;
}


int LevelDBStore::set(const std::string& key, const std::string& val) const {

    leveldb::WriteOptions options;
    options.sync = true;

    leveldb::Status status = kv_fp_->Put(options, key, val);
    if (status.ok()) return 0;

    return -1;
}

int LevelDBStore::del(const std::string& key) const {

    leveldb::WriteOptions options;
    options.sync = true;

    leveldb::Status status = kv_fp_->Delete(leveldb::WriteOptions(), key);
    if (status.ok()) return 0;

    return -1;
}


int LevelDBStore::range(const std::string& start, const std::string& end, uint64_t limit,
                        std::vector<std::string>& range_store) const {

    std::unique_ptr<leveldb::Iterator> it(kv_fp_->NewIterator(leveldb::ReadOptions()));
    uint64_t count = 0;
    leveldb::Options options;

    it->SeekToFirst();
    if (!start.empty())
        it->Seek(start);

    for (/* */; it->Valid(); it->Next()) {

        leveldb::Slice key = it->key();
        std::string key_str = key.ToString();

        // leveldb::Slice value = it->value();
        // std::string val_str = value.ToString();

        if (limit && ++count > limit) {
            break;
        }

        if (!end.empty() && options.comparator->Compare(key, end) > 0) {
            break;
        }

        range_store.push_back(key_str);
    }

    return 0;
}


int LevelDBStore::search(const std::string& search_key, uint64_t limit,
                         std::vector<std::string>& search_store) const {


    std::unique_ptr<leveldb::Iterator> it(kv_fp_->NewIterator(leveldb::ReadOptions()));
    uint64_t count = 0;
    leveldb::Options options;

    for (it->SeekToFirst(); it->Valid(); it->Next()) {

        leveldb::Slice key = it->key();
        std::string key_str = key.ToString();

        // leveldb::Slice value = it->value();
        // std::string val_str = value.ToString();

        if (key_str.find(search_key) != std::string::npos) {
            search_store.push_back(key_str);
        }

        if (limit && ++count > limit) {
            break;
        }
    }

    return 0;
}

} // namespace sisyphus
