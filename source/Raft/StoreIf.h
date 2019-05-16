/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __RAFT_STORE_IF_H__
#define __RAFT_STORE_IF_H__

#include <xtra_rhel.h>

#include <Protocol/gen-cpp/Client.pb.h>


// 用来进行实际业务数据存储的LevelDB实例

namespace sisyphus {

class StoreIf {

    __noncopyable__(StoreIf)

public:
    StoreIf() = default;
    ~StoreIf() = default;

    // 客户端查询使用，不涉及状态机的变更
    virtual int select_handle(const Client::StateMachineSelectOps::Request& request,
                              Client::StateMachineSelectOps::Response& response) const = 0;

    // 状态机执行日志的时候使用
    virtual int update_handle(const Client::StateMachineUpdateOps::Request& request) const = 0;

    virtual bool create_snapshot(uint64_t last_included_index, uint64_t last_included_term) const = 0;
    virtual bool load_snapshot(uint64_t& last_included_index, uint64_t& last_included_term) = 0;
};

} // namespace sisyphus

#endif // __RAFT_KV_STORE_H__
