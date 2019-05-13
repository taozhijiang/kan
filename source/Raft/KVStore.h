#ifndef __RAFT_KV_STORE_H__
#define __RAFT_KV_STORE_H__

#include <xtra_rhel.h>
#include <leveldb/db.h>

#include <system/ConstructException.h>


// 用来进行实际业务数据存储的LevelDB实例

namespace sisyphus {

class KVStore {

    __noncopyable__(KVStore)

public:
    explicit KVStore(const std::string& path);
    ~KVStore();

    int get(const std::string& key, std::string& val) const;
    int set(const std::string& key, const std::string& val) const;

protected:
    const std::string kv_path_;
    std::unique_ptr<leveldb::DB> kv_fp_;
};

} // namespace sisyphus

#endif // __RAFT_KV_STORE_H__
