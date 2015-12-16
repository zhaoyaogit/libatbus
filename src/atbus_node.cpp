﻿/**
 * @brief 所有channel文件的模式均为 c + channel<br />
 *        使用c的模式是为了简单、结构清晰并且避免异常<br />
 *        附带c++的部分是为了避免命名空间污染并且c++的跨平台适配更加简单
 */

#ifndef _MSC_VER

#include <sys/types.h>
#include <unistd.h>

#else
#pragma comment(lib, "Ws2_32.lib") 
#endif

#include <cstdio>
#include <assert.h>
#include <ctime>
#include <stdint.h>
#include <sstream>
#include <cstddef>
#include <cstring>
#include <cstdlib>

#include <common/string_oprs.h>

#include "detail/buffer.h"


#include "atbus_msg_handler.h"
#include "atbus_node.h"

#include "detail/libatbus_protocol.h"

namespace atbus {
    namespace detail {
        struct io_stream_channel_del {
            void operator()(channel::io_stream_channel* p) const {
                channel::io_stream_close(p);
                delete p;
            }
        };
    }

    node::node(): state_(state_t::CREATED), ev_loop_(NULL), static_buffer_(NULL) {
        event_timer_.sec = 0;
        event_timer_.usec = 0;
        event_timer_.node_sync_push = 0;
        event_timer_.father_opr_time_point = 0;
    }

    node::~node() {
        reset();
    }

    void node::default_conf(conf_t* conf) {
        conf->ev_loop = NULL;
        conf->children_mask = 0;
        conf->loop_times = 128;
        conf->ttl = 16; // 默认最长8次跳转

        conf->first_idle_timeout = ATBUS_MACRO_CONNECTION_CONFIRM_TIMEOUT;
        conf->ping_interval = 60;
        conf->retry_interval = 3;
        conf->fault_tolerant = 3;
        conf->backlog = ATBUS_MACRO_CONNECTION_BACKLOG;

        conf->msg_size = ATBUS_MACRO_MSG_LIMIT;
        conf->recv_buffer_size = ATBUS_MACRO_MSG_LIMIT * 32; // default for 3 times of ATBUS_MACRO_MSG_LIMIT = 2MB
        conf->send_buffer_size = ATBUS_MACRO_MSG_LIMIT;
        conf->send_buffer_number = 0;

        conf->flags.reset();
    }

    node::ptr_t node::create() {
        ptr_t ret(new node());
        if (!ret) {
            return ret;
        }

        ret->watcher_ = ret;
        return ret;
    }

    int node::init(bus_id_t id, const conf_t* conf) {
        reset();

        if (NULL == conf) {
            default_conf(&conf_);
        } else {
            conf_ = *conf;
        }

        self_ = endpoint::create(this, id, conf_.children_mask, get_pid(), get_hostname());
        if(!self_) {
            return EN_ATBUS_ERR_MALLOC;
        }
        // 复制全局路由表配置
        self_->set_flag(endpoint::flag_t::GLOBAL_ROUTER, conf_.flags.test(flag_t::EN_CONF_GLOBAL_ROUTER));

        static_buffer_ = detail::buffer_block::malloc(conf_.msg_size + detail::buffer_block::head_size(conf_.msg_size) + 16); // 预留crc32长度和vint长度);

        ev_loop_ = NULL;


        state_ = state_t::INITED;
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::start() {
        // 初始化时间
        event_timer_.sec = time(NULL);

        // 连接父节点
        if (!conf_.father_address.empty()) {
            if(!node_father_.node_) {
                // 如果父节点被激活了，那么父节点操作时间必须更新到非0值，以启用这个功能
                if(connect(conf_.father_address.c_str()) >= 0) {
                    event_timer_.father_opr_time_point = event_timer_.sec + conf_.first_idle_timeout;
                    state_ = state_t::CONNECTING_PARENT;
                } else {
                    event_timer_.father_opr_time_point = event_timer_.sec + conf_.retry_interval;
                    state_ = state_t::LOST_PARENT;
                }
            }
        } else {
            state_ = state_t::RUNNING;
        }

        return 0;
    }

    int node::reset() {
        // 这个函数可能会在析构时被调用，这时候不能使用watcher_.lock()
        if (conf_.flags.test(flag_t::EN_CONF_RESETTING)) {
            return EN_ATBUS_ERR_SUCCESS;
        }
        conf_.flags.set(flag_t::EN_CONF_RESETTING, true);

        // 所有连接断开
        for (detail::auto_select_map<std::string, connection::ptr_t>::type::iterator iter = proc_connections_.begin(); 
            iter != proc_connections_.end(); ++ iter) {
            if (iter->second) {
                iter->second->reset();
            }
        }
        proc_connections_.clear();

        // 销毁endpoint
        if (node_father_.node_) {
            node_father_.node_->reset();
            node_father_.node_.reset();
        }

        for (endpoint_collection_t::iterator iter = node_brother_.begin(); iter != node_brother_.end(); ++ iter) {
            if (iter->second) {
                iter->second->reset();
            }
        }
        node_brother_.clear();

        for (endpoint_collection_t::iterator iter = node_children_.begin(); iter != node_children_.end(); ++iter) {
            if (iter->second) {
                iter->second->reset();
            }
        }
        node_children_.clear();

        // 清空检测列表
        event_timer_.pending_check_list_.clear();

        // 基础数据
        iostream_channel_.reset();
        iostream_conf_.reset();

        if (NULL != ev_loop_) {
            ev_loop_ = NULL;
        }

        if (NULL != static_buffer_) {
            detail::buffer_block::free(static_buffer_);
            static_buffer_ = NULL;
        }
        
        conf_.flags.reset();
        state_ = state_t::CREATED;
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::proc(time_t sec, time_t usec) {
        if (sec > event_timer_.sec) {
            event_timer_.sec = sec;
            event_timer_.usec = usec;
        } else if (sec == event_timer_.sec && usec > event_timer_.usec) {
            event_timer_.usec = usec;
        }
        

        int ret = 0;
        // TODO 以后可以优化成event_fd通知，这样就不需要轮询了
        // 点对点IO流通道
        for (detail::auto_select_map<std::string, connection::ptr_t>::type::iterator iter = proc_connections_.begin(); iter != proc_connections_.end(); ++ iter) {
            ret += iter->second->proc(*this, sec, usec);
        }

        // 点对点IO流通道
        int loop_left = conf_.loop_times;
        while (iostream_channel_ && loop_left > 0 &&
            EN_ATBUS_ERR_EV_RUN == channel::io_stream_run(get_iostream_channel(), adapter::RUN_NOWAIT)) {
            --loop_left;
        }

        ret += conf_.loop_times - loop_left;

        // connection超时下线
        while (!event_timer_.connecting_list.empty() && event_timer_.connecting_list.front().first < sec) {
            evt_timer_t::timer_desc_ls<connection::ptr_t>::pair_type& top = event_timer_.connecting_list.front();

            // 已无效对象则忽略
            if (top.second && false == top.second->is_connected()) {
                top.second->reset();
                on_error(NULL, top.second.get(), EN_ATBUS_ERR_NODE_TIMEOUT, 0);
            }

            event_timer_.connecting_list.pop_front();
        }

        // 父节点操作
        if (!conf_.father_address.empty() && 0 != event_timer_.father_opr_time_point && event_timer_.father_opr_time_point < sec) {
            // 获取命令节点
            connection* ctl_conn = NULL;
            if (!node_father_.node_) {
                ctl_conn = self_->get_ctrl_connection(node_father_.node_.get());
            }

            // 父节点重连
            if (NULL == ctl_conn) {
                int res = connect(conf_.father_address.c_str());
                if (res < 0) {
                    on_error(NULL, NULL, res, 0);

                    event_timer_.father_opr_time_point = sec + conf_.retry_interval;
                } else {
                    // 下一次判定父节点连接超时再重新连接
                    event_timer_.father_opr_time_point = sec + conf_.first_idle_timeout;
                    state_ = state_t::CONNECTING_PARENT;
                }
            } else {
                int res = ping_endpoint(*node_father_.node_);
                if (res < 0) {
                    on_error(NULL, NULL, res, 0);
                }

                // ping包不需要重试
                event_timer_.father_opr_time_point = sec + conf_.ping_interval;
            }
        }

        // Ping包
        while(!event_timer_.ping_list.empty() && event_timer_.ping_list.front().first < sec) {
            endpoint::ptr_t ep = event_timer_.ping_list.front().second.lock();
            event_timer_.ping_list.pop_front();

            // 已移除对象则忽略
            if (!ep) {
                // 忽略错误
                ping_endpoint(*ep);
                event_timer_.ping_list.push_back(std::make_pair(sec + conf_.ping_interval, ep));
            }
        }

        // 节点同步协议-推送
        if (0 != event_timer_.node_sync_push && event_timer_.node_sync_push < sec) {
            // 发起子节点同步信息推送
            int res = push_node_sync();
            if (res < 0) {
                event_timer_.node_sync_push = sec + conf_.retry_interval;
            } else {
                event_timer_.node_sync_push = 0;
            }
        }


        // 检测队列
        if (!event_timer_.pending_check_list_.empty()) {
            std::list<endpoint::ptr_t> checked;
            checked.swap(event_timer_.pending_check_list_);

            for (std::list<endpoint::ptr_t>::iterator iter = checked.begin(); iter != checked.end(); ++ iter) {
                if (*iter) {
                    if(false == (*iter)->is_available()) {
                        (*iter)->reset();
                        remove_endpoint((*iter)->get_id());
                    }
                }
            }
        }
        return ret;
    }

    int node::listen(const char* addr_str) {
        connection::ptr_t conn = connection::create(this);
        if(!conn) {
            return EN_ATBUS_ERR_MALLOC;
        }

        int ret = conn->listen(addr_str);
        if (ret < 0) {
            return ret;
        }

        // 添加到self_里
        if(false == self_->add_connection(conn.get(), false)) {
            return EN_ATBUS_ERR_ALREADY_INITED;
        }

        // 记录监听地址
        self_->add_listen(conn->get_address().address);
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::connect(const char* addr_str) {
        connection::ptr_t conn = connection::create(this);
        if (!conn) {
            return EN_ATBUS_ERR_MALLOC;
        }

        // 内存通道和共享内存通道不允许协商握手，必须直接指定endpoint
        if (0 == UTIL_STRFUNC_STRNCASE_CMP("mem", addr_str, 3)) {
            return EN_ATBUS_ERR_ACCESS_DENY;
        } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("shm", addr_str, 3)) {
            return EN_ATBUS_ERR_ACCESS_DENY;
        }

        int ret = conn->connect(addr_str);
        if (ret < 0) {
            return ret;
        }

        // 添加到检测队列
        add_connection_timer(conn);
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::connect(const char* addr_str, endpoint* ep) {
        if(NULL == ep) {
            return EN_ATBUS_ERR_PARAMS;
        }

        connection::ptr_t conn = connection::create(this);
        if (!conn) {
            return EN_ATBUS_ERR_MALLOC;
        }

        int ret = conn->connect(addr_str);
        if (ret < 0) {
            return ret;
        }

        if (0 == UTIL_STRFUNC_STRNCASE_CMP("mem:", addr_str, 4) ||
            0 == UTIL_STRFUNC_STRNCASE_CMP("shm:", addr_str, 4)
            ) {
            if (ep->add_connection(conn.get(), true)) {
                return EN_ATBUS_ERR_SUCCESS;
            }
        } else {
            if (ep->add_connection(conn.get(), false)) {
                return EN_ATBUS_ERR_SUCCESS;
            }
        }
        
        return EN_ATBUS_ERR_BAD_DATA;
    }

    int node::disconnect(bus_id_t id) {
        if (node_father_.node_ && id == node_father_.node_->get_id()) {
            node_father_.node_->reset();
            node_father_.node_.reset();
            return EN_ATBUS_ERR_SUCCESS;
        }

        endpoint* ep = find_child(node_brother_, id);
        if (NULL != ep && ep->get_id() == id) {
            ep->reset();

            // 移除连接关系
            remove_child(node_brother_, id);
            // TODO 移除全局表
            return EN_ATBUS_ERR_SUCCESS;
        }

        ep = find_child(node_children_, id);
        if (NULL != ep && ep->get_id() == id) {
            ep->reset();

            // 移除连接关系
            remove_child(node_brother_, id);
            // TODO 移除全局表
            return EN_ATBUS_ERR_SUCCESS;
        }

        return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
    }

    int node::send_data(bus_id_t tid, int type, const void* buffer, size_t s) {
        if (s >= conf_.msg_size) {
            return EN_ATBUS_ERR_BUFF_LIMIT;
        }

        if(tid == get_id()) {
            // 发送给自己的数据直接回调数据接口
            on_recv_data(NULL, type, buffer, s);
            return EN_ATBUS_ERR_SUCCESS;
        }

        atbus::protocol::msg m;
        m.init(ATBUS_CMD_DATA_TRANSFORM_REQ, type, 0, alloc_msg_seq());

        if (NULL == m.body.make_body(m.body.forward)) {
            return EN_ATBUS_ERR_MALLOC;
        }

        m.body.forward->from = get_id();
        m.body.forward->to = tid;
        m.body.forward->content.ptr = buffer;
        m.body.forward->content.size = s;

        return send_msg(tid, m);
    }

    int node::send_msg(bus_id_t tid, atbus::protocol::msg& m) {
        if (tid == get_id()) {
            // 发送给自己的数据直接回调数据接口
            on_recv(NULL, &m, 0, 0);
            return EN_ATBUS_ERR_SUCCESS;
        }

        connection* conn = NULL;
        do {
            // 父节点单独判定，由于防止测试兄弟节点
            if (node_father_.node_ && is_parent_node(tid)) {
                conn = self_->get_data_connection(node_father_.node_.get());
            }

            // 兄弟节点(父节点会被判为可能是兄弟节点)
            if (is_brother_node(tid)) {
                endpoint* target = find_child(node_brother_, tid);
                if (NULL != target && target->is_child_node(tid)) {
                    conn = self_->get_data_connection(target);
                    break;
                }
                return EN_ATBUS_ERR_ATNODE_INVALID_ID;
            }

            // 子节点
            if (is_child_node(tid)) {
                endpoint* target = find_child(node_children_, tid);
                if (NULL != target && target->is_child_node(tid)) {
                    conn = self_->get_data_connection(target);
                    break;
                }
                return EN_ATBUS_ERR_ATNODE_INVALID_ID;
            }

            // 其他情况发给父节点
            if (node_father_.node_ && tid == node_father_.node_->get_id()) {
                conn = self_->get_data_connection(node_father_.node_.get());
                break;
            }
        } while (false);

        if (NULL == conn) {
            return EN_ATBUS_ERR_ATNODE_NO_CONNECTION;
        }

        if (NULL != m.body.forward) {
            m.body.forward->router.push_back(get_id());
        }

        return msg_handler::send_msg(*this, *conn, m);
    }

    endpoint* node::get_endpoint(bus_id_t tid) {
        if (is_parent_node(tid)) {
            return node_father_.node_.get();
        }

        // 直连兄弟节点
        if (is_brother_node(tid)) {
            endpoint* res = find_child(node_brother_, tid);
            if (NULL != res && res->get_id() == tid) {
                return res;
            }
            return NULL;
        }

        // 直连子节点
        if (is_child_node(tid)) {
            endpoint* res = find_child(node_brother_, tid);
            if (NULL != res && res->get_id() == tid) {
                return res;
            }
            return NULL;
        }

        return NULL;
    }

    int node::add_endpoint(endpoint::ptr_t ep) {
        if(!ep) {
            return EN_ATBUS_ERR_PARAMS;
        }

        if (this != ep->get_owner()) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // 父节点单独判定，由于防止测试兄弟节点
        if (ep->get_children_mask() > self_->get_children_mask() && ep->is_child_node(get_id())) {
            if (!node_father_.node_) {
                node_father_.node_ = ep;

                if (state_t::CONNECTING_PARENT == state_) {
                    state_ = state_t::RUNNING;
                }
                return EN_ATBUS_ERR_SUCCESS;
            } else {
                // 父节点只能有一个
                return EN_ATBUS_ERR_ATNODE_INVALID_ID;
            }
        }

        // 兄弟节点(父节点会被判为可能是兄弟节点)
        if (is_brother_node(ep->get_id())) {
            if (insert_child(node_brother_, ep)) {
                return EN_ATBUS_ERR_SUCCESS;
            } else {
                return EN_ATBUS_ERR_ATNODE_INVALID_ID;
            }
        }

        // 子节点
        if (is_child_node(ep->get_id())) {
            if(insert_child(node_children_, ep)) {
                // TODO 子节点上线上报
                return EN_ATBUS_ERR_SUCCESS;
            } else {
                return EN_ATBUS_ERR_ATNODE_INVALID_ID;
            }
        }

        return EN_ATBUS_ERR_ATNODE_INVALID_ID;
    }

    int node::remove_endpoint(bus_id_t tid) {
        // 父节点单独判定，由于防止测试兄弟节点
        if (is_parent_node(tid)) {
            node_father_.node_.reset();
            state_ = state_t::LOST_PARENT;
            return EN_ATBUS_ERR_SUCCESS;
        }

        // 兄弟节点(父节点会被判为可能是兄弟节点)
        if (is_brother_node(tid)) {
            if (remove_child(node_brother_, tid)) {
                return EN_ATBUS_ERR_SUCCESS;
            } else {
                return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
            }
        }

        // 子节点
        if (is_child_node(tid)) {
            if (remove_child(node_children_, tid)) {
                // TODO 子节点下线上报
                return EN_ATBUS_ERR_SUCCESS;
            } else {
                return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
            }
        }

        return EN_ATBUS_ERR_ATNODE_INVALID_ID;
    }

    adapter::loop_t* node::get_evloop() {
        if (NULL != ev_loop_) {
            return ev_loop_;
        }

        ev_loop_ = uv_default_loop();
        return ev_loop_;
    }

    bool node::is_child_node(bus_id_t id) const {
        return self_->is_child_node(id);
    }

    bool node::is_brother_node(bus_id_t id) const {
        if (node_father_.node_) {
            return self_->is_brother_node(id, node_father_.node_->get_children_mask());
        } else {
            return self_->is_brother_node(id, 0);
        }
    }

    bool node::is_parent_node(bus_id_t id) const {
        if (node_father_.node_) {
            return self_->is_parent_node(id, node_father_.node_->get_id(), node_father_.node_->get_children_mask());
        }

        return false;
    }

    int node::get_pid() {
#ifdef _MSC_VER
        return _getpid();
#else
        return getpid();
#endif
    }

    static std::string& host_name_buffer() {
        static std::string server_addr;
        return server_addr;
    }

    const std::string& node::get_hostname() {
        std::string& hn = host_name_buffer();
        if (!hn.empty()) {
            return hn;
        }

        // @see man gethostname
        char buffer[256] = {0};
        if(0 == gethostname(buffer, sizeof(buffer))) {
            hn = buffer;
        }
#ifdef _MSC_VER
        else {
            if (WSANOTINITIALISED == WSAGetLastError()) {
                WSADATA wsaData;
                WORD version = MAKEWORD(2, 0);
                if(0 == WSAStartup(version, &wsaData) && 0 == gethostname(buffer, sizeof(buffer))) {
                    hn = buffer;
                }
            }
        }
#endif

        return hn;
    }

    bool node::set_hostname(const std::string& hn) {
        std::string& h = host_name_buffer();
        if (h.empty()) {
            h = hn;
            return true;
        }

        return false;
    }

    bool node::add_proc_connection(connection::ptr_t conn) {
        if(!conn || conn->get_address().address.empty() || proc_connections_.end() != proc_connections_.find(conn->get_address().address)) {
            return false;
        }

        proc_connections_[conn->get_address().address] = conn;
        return true;
    }

    bool node::remove_proc_connection(const std::string& conn_key) {
        detail::auto_select_map<std::string, connection::ptr_t>::type::iterator iter = proc_connections_.find(conn_key);
        if (iter == proc_connections_.end()) {
            return false;
        }

        proc_connections_.erase(iter);
        return true;
    }

    bool node::add_connection_timer(connection::ptr_t conn) {
        if (!conn) {
            return false;
        }

        if (connection::state_t::DISCONNECTED == conn->get_status() || connection::state_t::DISCONNECTING == conn->get_status()) {
            return false;
        }

        // 如果处于握手阶段，发送节点关系逻辑并加入握手连接池并加入超时判定池
        if (false == conn->is_connected()) {
            event_timer_.connecting_list.push_back(std::make_pair(event_timer_.sec + conf_.first_idle_timeout, conn));
        }
        return true;
    }

    time_t node::get_timer_sec() const {
        return event_timer_.sec;
    }

    time_t node::get_timer_usec() const {
        return event_timer_.usec;
    }

    void node::on_recv(connection* conn, protocol::msg* m, int status, int errcode) {
        if (status < 0 || errcode < 0) {
            on_error(NULL, conn, status, errcode);

            if (NULL != conn) {
                endpoint* ep = conn->get_binding();
                if(NULL != ep) {
                    add_endpoint_fault(*ep);
                }
            }
            return;
        }

        // 内部协议处理
        int res = msg_handler::dispatch_msg(*this, conn, m, status, errcode);
        if (res < 0) {
            if (NULL != conn) {
                endpoint* ep = conn->get_binding();
                if (NULL != ep) {
                    add_endpoint_fault(*ep);
                }
            }

            return;
        }

        if (NULL == m) {
            return;
        }

        if (NULL != conn) {
            endpoint* ep = conn->get_binding();
            if (NULL != ep) {
                if (m->head.ret < 0) {
                    add_endpoint_fault(*ep);
                } else {
                    ep->clear_stat_fault();
                }
            }
        }
    }

    void node::on_recv_data(connection* conn, int type, const void* buffer, size_t s) const {
    }

    int node::on_error(const endpoint* ep, const connection* conn, int status, int errcode) {
        if (NULL == ep && NULL != conn) {
            ep = conn->get_binding();
        }

        if (event_msg_.on_error) {
            event_msg_.on_error(*this, ep, conn, status, errcode);
        }

        return status;
    }

    int node::on_disconnect(const connection* conn) {
        if (NULL == conn) {
            return EN_ATBUS_ERR_PARAMS;
        }
        
        // TODO 断线逻辑
        // conn->reset();
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_new_connection(connection* conn) {
        if (NULL == conn) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // 发送注册协议
        int ret = msg_handler::send_reg(ATBUS_CMD_NODE_REG_REQ, *this, *conn, 0, 0);
        if (ret < 0) {
            on_error(NULL, conn, ret, 0);
            conn->reset();
            return ret;
        }

        // 设置超时检测
        add_connection_timer(conn->watch());
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_shutdown(int reason) {
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::shutdown(int reason) {
        int ret = on_shutdown(reason);

        reset();
        return ret;
    }

    int node::ping_endpoint(endpoint& ep) {
        // 检测上一次ping是否返回
        if (0 != ep.get_stat_ping()) {
            if (add_endpoint_fault(ep)) {
                return EN_ATBUS_ERR_ATNODE_FAULT_TOLERANT;
            }
        }


        // 允许跳过未连接或握手完成的endpoint
        connection* ctl_conn = self_->get_ctrl_connection(&ep);
        if (NULL == ctl_conn) {
            return EN_ATBUS_ERR_SUCCESS;
        }

        // 出错则增加错误计数
        uint32_t ping_seq = msg_seq_alloc_.inc();
        int res = msg_handler::send_ping(*this, *ctl_conn, ping_seq);
        if (res < 0) {
            add_endpoint_fault(ep);
            return res;
        }

        ep.set_stat_ping(ping_seq);
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::push_node_sync() {
        // TODO 防止短时间内批量上报注册协议，所以合并上报数据包
        
        // TODO 给所有需要全局路由表的子节点下发数据
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::pull_node_sync() {
        // TODO 拉取全局节点信息表
        return EN_ATBUS_ERR_SUCCESS;
    }

    uint32_t node::alloc_msg_seq() {
        uint32_t ret = 0;
        while (!ret) {
            ret = msg_seq_alloc_.inc();
        }
        return ret;
    }

    void node::add_check_list(const endpoint::ptr_t& ep) {
        if (ep) {
            event_timer_.pending_check_list_.push_back(ep);
        }
    }

    endpoint* node::find_child(endpoint_collection_t& coll, bus_id_t id) {
        // key 保存为子域上界，所以第一个查找的节点要么直接是value，要么是value的子节点
        endpoint_collection_t::iterator iter = coll.lower_bound(id);
        if (iter == coll.end()) {
            return NULL;
        }

        if (iter->second->get_id() == id) {
            return iter->second.get();
        }

        // 不能直接发送到间接子节点，所以直接发给直接子节点由其转发即可
        if (iter->second->is_child_node(id)) {
            return iter->second.get();
        }

        return NULL;
    }

    bool node::insert_child(endpoint_collection_t& coll, endpoint::ptr_t ep) {
        if (!ep) {
            return false;
        }

        bus_id_t maskv = endpoint::get_children_max_id(ep->get_id(), ep->get_children_mask());

        // key 保存为子域上界，所以第一个查找的节点要么目标节点的父节点，要么子节点域交叉
        endpoint_collection_t::iterator iter = coll.lower_bound(ep->get_id());

        // 如果在所有子节点域外则直接添加
        if (iter == coll.end()) {
            // 有可能这个节点覆盖最后一个
            if (!coll.empty() && ep->is_child_node(coll.rbegin()->second->get_id())) {
                return false;
            }

            coll[maskv] = ep;
            return true;
        }

        // 如果是数据merge则出错退出
        if (iter->second->get_id() == ep->get_id()) {
            return false;
        }

        // 如果新节点是老节点的子节点，则失败退出
        if (iter->second->is_child_node(ep->get_id()) || ep->is_child_node(iter->second->get_id())) {
            return false;
        }

        // 如果有老节点是新节点的子节点，则失败退出
        --iter;
        if (iter != coll.end() && ep->is_child_node(iter->second->get_id())) {
            return false;
        }

        coll[maskv] = ep;
        return true;
    }

    bool node::remove_child(endpoint_collection_t& coll, bus_id_t id) {
        endpoint_collection_t::iterator iter = coll.lower_bound(id);
        if (iter == coll.end()) {
            return false;
        }

        if (iter->second->get_id() != id) {
            return false;
        }

        coll.erase(iter);
        return true;
    }

    bool node::add_endpoint_fault(endpoint& ep) {
        size_t fault_count = ep.add_stat_fault();
        if (fault_count > conf_.fault_tolerant) {
            remove_endpoint(ep.get_id());
            return true;
        }

        return false;
    }

    channel::io_stream_channel* node::get_iostream_channel() {
        if(iostream_channel_) {
            return iostream_channel_.get();
        }
        iostream_channel_ = std::shared_ptr<channel::io_stream_channel>(new channel::io_stream_channel(), detail::io_stream_channel_del());

        channel::io_stream_init(iostream_channel_.get(), get_evloop(), get_iostream_conf());
        iostream_channel_->data = this;

        // callbacks
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_ACCEPTED] = connection::iostream_on_accepted;
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_CONNECTED] = connection::iostream_on_connected;
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_DISCONNECTED] = connection::iostream_on_disconnected;
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_RECVED] = connection::iostream_on_recv_cb;

        return iostream_channel_.get();
    }

    node::ptr_t node::get_watcher() {
        return watcher_.lock();
    }

    channel::io_stream_conf* node::get_iostream_conf() {
        if (iostream_conf_) {
            return iostream_conf_.get();
        }

        iostream_conf_.reset(new channel::io_stream_conf());
        channel::io_stream_init_configure(iostream_conf_.get());

        // 接收大小和msg size一致即可，可以只使用一块静态buffer
        iostream_conf_->recv_buffer_limit_size = conf_.msg_size;
        iostream_conf_->recv_buffer_max_size = conf_.msg_size;

        iostream_conf_->send_buffer_static = conf_.send_buffer_number;
        iostream_conf_->send_buffer_max_size = conf_.send_buffer_size;
        iostream_conf_->send_buffer_limit_size = conf_.msg_size;
        iostream_conf_->confirm_timeout = conf_.first_idle_timeout;
        iostream_conf_->backlog = conf_.backlog;

        return iostream_conf_.get();
    }
}
