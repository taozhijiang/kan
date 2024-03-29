/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <xtra_rhel.h>

#include <cinttypes>
#include <memory>
#include <vector>

#include <Protocol/gen-cpp/Raft.pb.h>
#include <Protocol/gen-cpp/RaftLogMetadata.pb.h>

#ifndef __RAFT_LOGIF_H__
#define __RAFT_LOGIF_H__

namespace kan {


class LogIf {

    __noncopyable__(LogIf)

public:
    typedef kan::Raft::Entry                   Entry;
    typedef kan::RaftLogMetadata::Metadata     LogMeta;

    typedef std::shared_ptr<Entry>                  EntryPtr;

    LogIf() = default;
    virtual ~LogIf() = default;

    virtual std::pair<uint64_t, uint64_t>
    append(const std::vector<EntryPtr>& newEntries) = 0;

    virtual EntryPtr entry(uint64_t index) const = 0;
    virtual bool     entries(uint64_t start, std::vector<EntryPtr>& entries, uint64_t limit = 0) const = 0;
    virtual std::pair<uint64_t, uint64_t>
    last_term_and_index() const = 0;

    virtual uint64_t start_index() const = 0;
    virtual uint64_t last_index() const = 0;

    // 截取日志
    virtual void truncate_prefix(uint64_t start_index) = 0;
    virtual void truncate_suffix(uint64_t last_index) = 0;

    virtual int meta_data(LogMeta* meta_data) const = 0;
    virtual int set_meta_data(const LogMeta& meta) const = 0;

    virtual uint64_t meta_commit_index() const = 0;
    virtual uint64_t meta_apply_index() const = 0;

    virtual int set_meta_commit_index(uint64_t commit_index) const = 0;
    virtual int set_meta_apply_index(uint64_t apply_index) const = 0;

};


} // end namespace kan

#endif // __RAFT_LOGIF_H__
