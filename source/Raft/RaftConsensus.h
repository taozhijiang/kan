#ifndef __RAFT_RAFT_CONSENSUS_H__
#define __RAFT_RAFT_CONSENSUS_H__

#include <xtra_rhel.h>

#include <message/ProtoBuf.h>
#include <Protocol/gen-cpp/Raft.pb.h>

#include <Raft/LogIf.h>

namespace sisyphus {

class RaftConsensus {

public:
    RaftConsensus() {
    }

    ~RaftConsensus() {}

    bool init();

public:

    int do_handle_request_vote(
        const Raft::RequestVoteOps::Request& request,
        Raft::RequestVoteOps::Response& response);

    int do_handle_append_entries(
        const Raft::AppendEntriesOps::Request& request,
        Raft::AppendEntriesOps::Response& response);

    int do_handle_install_snapshot(
        const Raft::InstallSnapshotOps::Request& request,
        Raft::InstallSnapshotOps::Response& response);


private:
    std::shared_ptr<LogIf> log_;
};

} // namespace sisyphus

#endif // __RAFT_RAFT_CONSENSUS_H__
