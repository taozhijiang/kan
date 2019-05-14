/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __RAFT_LEVELDB_STORE_H__
#define __RAFT_LEVELDB_STORE_H__

#include <xtra_rhel.h>
#include <leveldb/db.h>

#include <Raft/StoreIf.h>
#include <system/ConstructException.h>


// 用来进行实际业务数据存储的LevelDB实例

namespace sisyphus {

class LevelDBStore : public StoreIf {

public:
    explicit LevelDBStore(const std::string& path);
    ~LevelDBStore();

    int select_handle(const Client::StateMachineSelectOps::Request& request,
                      Client::StateMachineSelectOps::Response& response) const override;
    int update_handle(const Client::StateMachineUpdateOps::Request& request) const override;

private:

    int get(const std::string& key, std::string& val) const;
    int set(const std::string& key, const std::string& val) const;
    int del(const std::string& key) const;

    int range(const std::string& start, const std::string& end, uint64_t limit,
              std::vector<std::string>& range_store) const;
    int search(const std::string& search_key, uint64_t limit,
               std::vector<std::string>& search_store) const;

protected:
    const std::string kv_path_;
    std::unique_ptr<leveldb::DB> kv_fp_;
};

} // namespace sisyphus

#endif // __RAFT_LEVELDB_STORE_H__
