/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#include <Network/NetServer.h>
#include <Network/TcpConnAsync.h>

#include <scaffold/Setting.h>
#include <scaffold/Status.h>

#include <string/StrUtil.h>

#include <Captain.h>
using kan::Captain;

namespace tzrpc {


bool NetConf::load_conf(std::shared_ptr<libconfig::Config> conf_ptr) {
    const auto& conf = *conf_ptr;
    return load_conf(conf);
}

bool NetConf::load_conf(const libconfig::Config& conf) {

    conf.lookupValue("rpc.network.bind_addr", bind_addr_);
    conf.lookupValue("rpc.network.bind_port", bind_port_);
    if (bind_addr_.empty() || bind_port_ <= 0) {
        roo::log_err("invalid rpc.network. bind_addr %s & bind_port %d.",
                     bind_addr_.c_str(), bind_port_);
        return false;
    }

    std::string ip_list;
    conf.lookupValue("rpc.network.safe_ip", ip_list);
    if (!ip_list.empty()) {
        std::vector<std::string> ip_vec;
        std::set<std::string> ip_set;
        boost::split(ip_vec, ip_list, boost::is_any_of(";"));
        for (std::vector<std::string>::iterator it = ip_vec.begin(); it != ip_vec.cend(); ++it) {
            std::string tmp = boost::trim_copy(*it);
            if (tmp.empty())
                continue;

            ip_set.insert(tmp);
        }

        std::swap(ip_set, safe_ip_);
    }

    if (!safe_ip_.empty()) {
        roo::log_warning("safe_ip not empty, totally contain %d items.",
                         static_cast<int>(safe_ip_.size()));
    }

    conf.lookupValue("rpc.network.backlog_size", backlog_size_);
    if (backlog_size_ < 0) {
        roo::log_err("invalid rpc.network.backlog_size %d.", backlog_size_);
        return false;
    }

    conf.lookupValue("rpc.network.io_thread_pool_size", io_thread_number_);
    if (io_thread_number_ < 0) {
        roo::log_err("invalid rpc.network.io_thread_number %d", io_thread_number_);
        return false;
    }

    conf.lookupValue("rpc.network.ops_cancel_time_out", ops_cancel_time_out_);
    if (ops_cancel_time_out_ < 0) {
        roo::log_err("invalid rpc.network.ops_cancel_time_out %d.", ops_cancel_time_out_);
        return false;
    }

    conf.lookupValue("rpc.network.session_cancel_time_out", session_cancel_time_out_);
    if (session_cancel_time_out_ < 0) {
        roo::log_err("invalid rpc.network.session_cancel_time_out %d.", session_cancel_time_out_);
        return false;
    }

    conf.lookupValue("rpc.network.service_enable", service_enabled_);
    conf.lookupValue("rpc.network.service_speed", service_speed_);
    if (service_speed_ < 0) {
        roo::log_err("invalid rpc.network.service_speed value %d.", service_speed_);
        return false;
    }

    conf.lookupValue("rpc.network.service_concurrency", service_concurrency_);
    if (service_concurrency_ < 0) {
        roo::log_err("invalid rpc.network.service_concurrency value %d.", service_concurrency_);
        return false;
    }

    // 如果是0，就不限制
    conf.lookupValue("rpc.network.send_max_msg_size", send_max_msg_size_);
    conf.lookupValue("rpc.network.recv_max_msg_size", recv_max_msg_size_);
    /*actual sizeof RpcRequestMessage, RpcResponseMessage*/
    if ((send_max_msg_size_ != 0 && send_max_msg_size_ <= 32) ||
        (recv_max_msg_size_ != 0 && recv_max_msg_size_ <= 32)) {
        roo::log_err("invalid rpc_network.send,recv max_msg_size %d, %d.",
                     send_max_msg_size_, recv_max_msg_size_);
        return false;
    }

    roo::log_info("NetConf conf parse successfully!");
    return true;
}

void NetConf::timed_feed_token_handler(const boost::system::error_code& ec) {

    if (service_speed_ == 0) {
        roo::log_warning("Unlock speed limit, so close the feed timer.");
        timed_feed_token_.reset();
        return;
    }

    // 恢复token
    feed_service_token();

    // 再次启动定时器
    timed_feed_token_->expires_from_now(seconds(1)); // 1sec
    timed_feed_token_->async_wait(
        std::bind(&NetConf::timed_feed_token_handler, this, std::placeholders::_1));
}


bool NetServer::init() {

    auto setting_ptr = Captain::instance().setting_ptr_->get_setting();

    // protect cfg race conditon
    std::lock_guard<std::mutex> lock(conf_.lock_);
    if (!conf_.load_conf(setting_ptr)) {
        roo::log_err("Load setting from roo::Setting failed.");
        return false;
    }

    // 上面的conf_参数已经经过合法性的检验了
    ep_ = boost::asio::ip::tcp::endpoint(boost::asio::ip::address::from_string(conf_.bind_addr_), conf_.bind_port_);
    roo::log_warning("create listen endpoint for %s:%d",
                     conf_.bind_addr_.c_str(), conf_.bind_port_);

    roo::log_info("socket/session conn cancel time_out: %d secs, enabled: %s.",
                  conf_.ops_cancel_time_out_,
                  conf_.ops_cancel_time_out_ > 0 ? "true" : "false");

    if (conf_.service_speed_) {
        conf_.timed_feed_token_.reset(new steady_timer(io_service_)); // 1sec
        if (!conf_.timed_feed_token_) {
            roo::log_err("Create timed_feed_token_ failed!");
            return false;
        }

        conf_.timed_feed_token_->expires_from_now(seconds(1));
        conf_.timed_feed_token_->async_wait(
            std::bind(&NetConf::timed_feed_token_handler, &conf_, std::placeholders::_1));
    }

    roo::log_info("rpc.network service enabled: %s, speed: %d tps.",
                  conf_.service_enabled_ ? "true" : "false",
                  conf_.service_speed_);

    if (!io_service_threads_.init_threads(
            std::bind(&NetServer::io_service_run, this, std::placeholders::_1),
            conf_.io_thread_number_)) {
        roo::log_err("NetServer::io_service_run init task failed.");
        return false;
    }

    // 注册配置动态更新的回调函数
    Captain::instance().setting_ptr_->attach_runtime_callback(
        "NetServer",
        std::bind(&NetServer::module_runtime, this,
                  std::placeholders::_1));

    // 系统状态展示相关的初始化
    Captain::instance().status_ptr_->attach_status_callback(
        "NetServer",
        std::bind(&NetServer::module_status, this,
                  std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    roo::log_warning("NetServer initialize successfully.");
    return true;
}

// accept stuffs
void NetServer::do_accept() {

    SocketPtr sock_ptr(new boost::asio::ip::tcp::socket(io_service_));
    acceptor_->async_accept(*sock_ptr,
                            std::bind(&NetServer::accept_handler, this,
                                      std::placeholders::_1, sock_ptr));
}


void NetServer::accept_handler(const boost::system::error_code& ec, SocketPtr sock_ptr) {

    do {

        if (ec) {
            roo::log_err("Recevied error when accept client with {%d} %s.", ec.value(), ec.message().c_str());
            break;
        }

        boost::system::error_code ignore_ec;
        auto remote = sock_ptr->remote_endpoint(ignore_ec);
        if (ignore_ec) {
            roo::log_err("Retrieve remote client info failed with{%d} %s.", ignore_ec.value(), ignore_ec.message().c_str());
            break;
        }

        std::string remote_ip = remote.address().to_string(ignore_ec);
        roo::log_info("Remote Client Info %s:%d", remote_ip.c_str(), remote.port());


        if (!conf_.check_safe_ip(remote_ip)) {
            roo::log_err("Check SafeIp failed for: %s", remote_ip.c_str());

            sock_ptr->shutdown(boost::asio::socket_base::shutdown_both, ignore_ec);
            sock_ptr->close(ignore_ec);
            break;
        }

        if (!conf_.get_service_token()) {
            roo::log_err("Request network speed token failed, current setting enabled: %s, speed: %d.",
                         conf_.service_enabled_ ? "true" : "false", conf_.service_speed_);

            sock_ptr->shutdown(boost::asio::socket_base::shutdown_both, ignore_ec);
            sock_ptr->close(ignore_ec);
            break;
        }

        if (conf_.service_concurrency_ != 0 &&
            conf_.service_concurrency_ < TcpConnAsync::current_concurrency_) {
            roo::log_err("Service Concurrency limit error, current setting limit: %d, and already connections: %d.",
                         conf_.service_concurrency_, TcpConnAsync::current_concurrency_.load());
            sock_ptr->shutdown(boost::asio::socket_base::shutdown_both, ignore_ec);
            sock_ptr->close(ignore_ec);
            break;
        }

        TcpConnAsyncPtr new_conn = std::make_shared<TcpConnAsync>(sock_ptr, *this);
        new_conn->start();

    } while (0);

    // 再次启动接收异步请求
    do_accept();
}


void NetServer::io_service_run(roo::ThreadObjPtr ptr) {

    while (true) {

        if (unlikely(ptr->status_ == roo::ThreadStatus::kTerminating)) {
            roo::log_err("io_service thread %#lx is about to terminating...", (long)pthread_self());
            break;
        }

        // 线程启动
        if (unlikely(ptr->status_ == roo::ThreadStatus::kSuspend)) {
            ::usleep(1 * 1000 * 1000);
            continue;
        }

        roo::log_warning("io_service thread %#lx about to loop...", (long)pthread_self());
        boost::system::error_code ec;
        io_service_.run(ec);

        if (ec) {
            roo::log_err("io_service stopped...");
            break;
        }
    }

    ptr->status_ = roo::ThreadStatus::kDead;
    roo::log_warning("io_service thread %#lx is about to terminate ... ", (long)pthread_self());

    return;
}



int NetServer::module_status(std::string& module, std::string& name, std::string& val) {

    module = "tzrpc";
    name = "NetServer";

    std::stringstream ss;

    ss << "\t" << "instance_name: " << instance_name_ << std::endl;
    ss << "\t" << "service_addr: " << conf_.bind_addr_ << "@" << conf_.bind_port_ << std::endl;
    ss << "\t" << "backlog_size: " << conf_.backlog_size_ << std::endl;
    ss << "\t" << "io_thread_pool_size: " << conf_.io_thread_number_ << std::endl;
    ss << "\t" << "safe_ips: ";

    {
        // protect cfg race conditon
        std::lock_guard<std::mutex> lock(conf_.lock_);
        for (auto iter = conf_.safe_ip_.begin(); iter != conf_.safe_ip_.end(); ++iter) {
            ss << *iter << ", ";
        }
        ss << std::endl;
    }

    ss << "\t" << std::endl;

    ss << "\t" << "service_enabled: " << (conf_.service_enabled_  ? "true" : "false") << std::endl;
    ss << "\t" << "service_speed_limit(tps): " << conf_.service_speed_ << std::endl;
    ss << "\t" << "service_concurrency: " << conf_.service_concurrency_ << std::endl;
    ss << "\t" << "session_cancel_time_out: " << conf_.session_cancel_time_out_ << std::endl;
    ss << "\t" << "ops_cancel_time_out: " << conf_.ops_cancel_time_out_ << std::endl;

    val = ss.str();
    return 0;
}


int NetServer::module_runtime(const libconfig::Config& cfg) {

    NetConf conf{};
    if (!conf.load_conf(cfg)) {
        roo::log_err("Load new setting for NetServer failed.");
        return -1;
    }

    if (conf_.session_cancel_time_out_ != conf.session_cancel_time_out_) {
        roo::log_warning("update session_cancel_time_out from %d to %d.",
                         conf_.session_cancel_time_out_, conf.session_cancel_time_out_);
        conf_.session_cancel_time_out_ = conf.session_cancel_time_out_;
    }

    if (conf_.ops_cancel_time_out_ != conf.ops_cancel_time_out_) {
        roo::log_warning("update ops_cancel_time_out from %d to %d.",
                         conf_.ops_cancel_time_out_, conf.ops_cancel_time_out_);
        conf_.ops_cancel_time_out_ = conf.ops_cancel_time_out_;
    }

    {
        roo::log_warning("about to swap SafeIP ...");

        // protect cfg race conditon
        std::lock_guard<std::mutex> lock(conf_.lock_);
        conf_.safe_ip_.swap(conf.safe_ip_);
    }

    if (conf_.service_speed_ != conf.service_speed_) {
        roo::log_warning("update service_speed from %d to %d.",
                         conf_.service_speed_, conf.service_speed_);
        conf_.service_speed_ = conf.service_speed_;

        // 检查定时器是否存在
        if (conf_.service_speed_) {

            // 直接重置定时器，无论有没有
            conf_.timed_feed_token_.reset(new steady_timer(io_service_)); // 1sec
            if (!conf_.timed_feed_token_) {
                roo::log_err("Create timed_feed_token for speed limit use failed.");
                return -1;
            }

            conf_.timed_feed_token_->expires_from_now(seconds(1));
            conf_.timed_feed_token_->async_wait(
                std::bind(&NetConf::timed_feed_token_handler, &conf_, std::placeholders::_1));
        } else { // speed == 0
            if (conf_.timed_feed_token_) {
                boost::system::error_code ignore_ec;
                conf_.timed_feed_token_->cancel(ignore_ec);
                conf_.timed_feed_token_.reset();
            }
        }
    }

    roo::log_warning("service speed limit enabled: %s, speed: %d.",
                     conf_.service_enabled_ ? "true" : "false",
                     conf_.service_speed_);

    if (conf_.service_concurrency_ != conf.service_concurrency_) {
        roo::log_err("update service_concurrency from %d to %d.",
                     conf_.service_concurrency_, conf.service_concurrency_);
        conf_.service_concurrency_ = conf.service_concurrency_;
    }

    if (conf_.service_concurrency_ != 0) {
        roo::log_warning("Service Concurrency limit %d.",  conf_.service_concurrency_);
    }

    return 0;
}



} // end namespace tzrpc
