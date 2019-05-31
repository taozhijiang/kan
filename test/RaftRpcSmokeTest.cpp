#include <iostream>
#include <string>

#include <gmock/gmock.h>
using namespace ::testing;

#include <roo/other/Log.h>

#include <Protocol/gen-cpp/Raft.pb.h>
#include <Protocol/Common.h>
#include <RpcClient.h>

#include <roo/message/ProtoBuf.h>

using namespace kan;
using namespace tzrpc_client;

class RaftRpcStub : public ::testing::Test {

protected:
    void SetUp() {
    }

    void TearDown() {
    }

public:

    RaftRpcStub():
        client_("127.0.0.1", 10801) {
    }

    virtual ~RaftRpcStub() {
    }

    RpcClient client_;
};


TEST_F(RaftRpcStub, RpcDispatchTest) {

    Raft::RequestVoteOps::Request  request {};
    Raft::RequestVoteOps::Response response {};

    request.set_candidate_id(10801);
    request.set_term(1);
    request.set_last_log_term(1);
    request.set_last_log_index(1);

    std::string from;
    std::string to;

    ASSERT_THAT(roo::ProtoBuf::marshalling_to_string(request, &from), Eq(true));
    RpcClientStatus code = client_.call_RPC(tzrpc::ServiceID::RAFT_SERVICE, Raft::OpCode::kRequestVote,
                                 from, to);

    ASSERT_THAT(code, Eq(RpcClientStatus::OK));

    ASSERT_THAT(roo::ProtoBuf::unmarshalling_from_string(to, &response), Eq(true));

    LOG(INFO) << roo::ProtoBuf::dump(response);
}
