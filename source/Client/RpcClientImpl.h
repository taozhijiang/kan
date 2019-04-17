/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __RPC_CLIENT_IMPL_H__
#define __RPC_CLIENT_IMPL_H__

#include <boost/asio/steady_timer.hpp>
using boost::asio::steady_timer;

#include <other/Log.h>
using roo::log_api;

#include <concurrency/IoService.h>

#include <Client/include/RpcClientStatus.h>
#include <Client/include/RpcClient.h>

namespace sisyphus {

class RpcRequestMessage;
class RpcResponseMessage;
class Message;

}

namespace sisyphus_client {

class TcpConnSync;

///////////////////////////
//
// 实现类 RpcClientImpl
//
//////////////////////////

class RpcClientImpl: public std::enable_shared_from_this<RpcClientImpl> {
public:
    RpcClientImpl(const RpcClientSetting& client_setting):
        client_setting_(client_setting),
        io_service_(),
        time_start_(0),
        was_timeout_(false),
        rpc_call_timer_(),
        conn_() {
    }

    ~RpcClientImpl();

    bool init();

    RpcClientStatus call_RPC(uint16_t service_id, uint16_t opcode,
                             const std::string& payload, std::string& respload,
                             uint32_t timeout_sec);

private:
    bool send_rpc_message(const sisyphus::RpcRequestMessage& rpc_request_message);
    bool recv_rpc_message(sisyphus::Message& net_message);

    RpcClientSetting client_setting_;

    std::unique_ptr<roo::IoService> io_service_;
    //
    // rpc调用超时相关的配置
    //
    time_t time_start_;        // 请求创建的时间
    bool was_timeout_;
    std::unique_ptr<steady_timer> rpc_call_timer_;
    void set_rpc_call_timeout(uint32_t msec);
    void rpc_call_timeout(const boost::system::error_code& ec);

    // 请求到达后按照需求自动创建
    std::shared_ptr<TcpConnSync> conn_;
};


} // end namespace sisyphus_client


#endif // __RPC_CLIENT_IMPL_H__
