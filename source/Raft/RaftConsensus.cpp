#include <other/Log.h>
using roo::log_api;


#include <Raft/LevelDBLog.h>

#include "RaftConsensus.h"

namespace sisyphus {


bool RaftConsensus::init() {

    log_ = std::make_shared<LevelDBLog>("./log_meta");

    return true;
}

int RaftConsensus::do_handle_request_vote(
    const Raft::RequestVoteOps::Request& request,
    Raft::RequestVoteOps::Response& response) {

    response.set_term(request.term());
    response.set_vote_granted(true);

    return 0;
}

int RaftConsensus::do_handle_append_entries(
    const Raft::AppendEntriesOps::Request& request,
    Raft::AppendEntriesOps::Response& response) {

    return 0;
}

int RaftConsensus::do_handle_install_snapshot(
    const Raft::InstallSnapshotOps::Request& request,
    Raft::InstallSnapshotOps::Response& response) {

    return 0;
}

} // namespace sisyphus
