/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <xtra_rhel.h>
#include <system/ConstructException.h>

#include <Raft/Peer.h>


#include <Captain.h>
#include <concurrency/IoService.h>

namespace kan {

using tzrpc_client::RpcClientStatus;
using tzrpc_client::RpcClientSetting;

Peer::Peer(uint64_t id,
           const std::string& addr, uint16_t port, const rpc_handler_t& handler) :
    id_(id),
    addr_(addr),
    port_(port),
    handler_(handler),
    rpc_client_(),
    rpc_proxy_(),
    next_index_(0),
    match_index_(0),
    latest_epoch_(0) {

    RpcClientSetting setting{};
    setting.serv_addr_ = addr_;
    setting.serv_port_ = port_;
    setting.io_service_ = Captain::instance().io_service_ptr_->io_service_ptr();
    rpc_proxy_  = make_unique<RpcClient>(setting);

    // add handler_ for async call use
    setting.handler_ = handler_;
    rpc_client_ = make_unique<RpcClient>(setting);

    if (!rpc_client_ || !rpc_proxy_)
        throw roo::ConstructException("Create RpClient failed.");
}

int Peer::send_raft_RPC(uint16_t service_id, uint16_t opcode, const std::string& payload) const {
    RpcClientStatus status = rpc_client_->call_RPC(service_id, opcode, payload);
    return status == RpcClientStatus::OK ? 0 : -1;
}


int Peer::proxy_client_RPC(uint16_t service_id, uint16_t opcode,
                           const std::string& payload, std::string& respload) const {
    RpcClientStatus status = rpc_proxy_->call_RPC(service_id, opcode, payload, respload);
    return status == RpcClientStatus::OK ? 0 : -1;
}

std::ostream& operator<<(std::ostream& os, const Peer& peer) {
    os << peer.str() << std::endl;
    return os;
}


} // namespace kan

