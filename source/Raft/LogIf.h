
#include <cinttypes>
#include <memory>
#include <vector>

#include <Protocol/gen-cpp/Raft.pb.h>
#include <Protocol/gen-cpp/RaftLogMetadata.pb.h>

#ifndef __RAFT_LOGIF_H__
#define __RAFT_LOGIF_H__

namespace sisyphus {


class LogIf {

    __noncopyable__(LogIf)

public:
    typedef sisyphus::Raft::Entry                   Entry;
    typedef sisyphus::RaftLogMetadata::Metadata     LogMeta;

    typedef std::shared_ptr<Entry>                  EntryPtr;

    LogIf() = default;
    virtual ~LogIf() = default;

    virtual std::pair<uint64_t, uint64_t>
    append(const std::vector<EntryPtr>& newEntries) = 0;

    virtual EntryPtr get_entry(uint64_t index) const = 0;
    virtual EntryPtr get_last_entry() const = 0;

    virtual uint64_t start_index() const = 0;
    virtual uint64_t last_index() const = 0;

    // 截取日志
    virtual void truncate_prefix(uint64_t start_index) = 0;
    virtual void truncate_suffix(uint64_t last_index) = 0;

    virtual int update_meta_data(const LogMeta& meta) const = 0;
    virtual int read_meta_data(LogMeta* meta_data) const = 0;
};


} // namespace LogIf

#endif // __RAFT_LOGIF_H__
