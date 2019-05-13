
#include <sstream>
#include <iostream>

#include <message/ProtoBuf.h>
#include <RpcClient.h>
#include <Protocol/Common.h>
#include <Protocol/gen-cpp/Client.pb.h>

static void usage() {
    std::stringstream ss;

    ss << std::endl
       << "./clientOps get <key>      " << std::endl
       << "            set <key> <val>" << std::endl
       << std::endl;
    
    std::cout << ss.str();

    ::exit(EXIT_SUCCESS);
}

static std::string random_str() {

    std::stringstream ss;
    ss << "message with random [" << ::random() << "] include";
    return ss.str();
}

const std::string srv_addr = "127.0.0.1";
const uint16_t    srv_port = 10801;

using namespace tzrpc_client;

int main(int argc, char* argv[]) {
    
    if(argc < 3) 
        usage();
    
    RpcClient client(srv_addr, srv_port);
    
    if(::strncmp(argv[1], "get", 3) == 0) {
    
        std::string key = argv[2];
        std::string mar_str;
        sisyphus::Client::StateMachineReadOps::Request request;
        request.set_key(key);
        if(!roo::ProtoBuf::marshalling_to_string(request, &mar_str)) {
            std::cerr << "marshalling message failed." << std::endl;
            return -1;
        }

        std::string resp_str;
        auto status = client.call_RPC(tzrpc::ServiceID::CLIENT_SERVICE,
                                      sisyphus::Client::OpCode::kRead,
                                      mar_str, resp_str);
        if(status != RpcClientStatus::OK) {
            std::cerr << "call failed, return code [" << static_cast<uint8_t>(status) << "]" << std::endl;
            return -1;
        }

        sisyphus::Client::StateMachineReadOps::Response response;
        if(!roo::ProtoBuf::unmarshalling_from_string(resp_str, &response)) {
            std::cerr << "unmarshalling message failed." << std::endl;
            return -1;
        }

        if(!response.has_code() || response.code() != 0 ) {
            std::cerr << "response code check error" << std::endl;
            return -1;
        }
        
        std::cout << "GOOD, return: " << response.val() << std::endl;

    } else if(::strncmp(argv[1], "set", 3) == 0) {

        if(argc < 4)
            usage();

        std::string key = argv[2];
        std::string val = argv[3];

        std::string mar_str;
        sisyphus::Client::StateMachineWriteOps::Request request;
        request.set_key(key);
        request.set_val(val);
        if(!roo::ProtoBuf::marshalling_to_string(request, &mar_str)) {
            std::cerr << "marshalling message failed." << std::endl;
            return -1;
        }

        std::string resp_str;
        auto status = client.call_RPC(tzrpc::ServiceID::CLIENT_SERVICE,
                                      sisyphus::Client::OpCode::kWrite,
                                      mar_str, resp_str);
        if(status != RpcClientStatus::OK) {
            std::cerr << "call failed, return code [" << static_cast<uint8_t>(status) << "]" << std::endl;
            return -1;
        }

        sisyphus::Client::StateMachineWriteOps::Response response;
        if(!roo::ProtoBuf::unmarshalling_from_string(resp_str, &response)) {
            std::cerr << "unmarshalling message failed." << std::endl;
            return -1;
        }

        if(!response.has_code() || response.code() != 0 ) {
            std::cerr << "response code check error" << std::endl;
            return -1;
        }
        
        std::cout << "GOOD, write return ok" << std::endl;

    } else {
        usage();
    }

 
    return 0;
}
