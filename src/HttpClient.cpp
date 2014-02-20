/*
 * Part of HTTPP.
 *
 * Distributed under the 3-clause BSD licence (See LICENCE.TXT file at the
 * project root).
 *
 * Copyright (c) 2014 Thomas Sanchez.  All rights reserved.
 *
 */

#include "httpp/HttpClient.hpp"

#include <iostream>
#include <mutex>
#include <stdexcept>

#include <curl/curl.h>

#include "http/client/Connection.hpp"

static std::once_flag curl_init_flag;
static void init_curl()
{
    if (curl_global_init(CURL_GLOBAL_ALL) != 0)
    {
        throw std::runtime_error("Cannot initialize curl");
    }
}

namespace HTTPP
{

using HTTP::client::detail::Connection;
using HTTP::Method;

struct HttpClient::Manager
{
    Manager(UTILS::ThreadPool& pool)
    : handler(curl_multi_init())
    , pool(pool)
    , strand(pool.getService())
    , timer(pool.getService())
    {
        if (!handler)
        {
            throw std::runtime_error("Cannot initialize curl multi handle");
        }

        manager_setopt(handler, CURLMOPT_SOCKETFUNCTION, &sock_cb);
        manager_setopt(handler, CURLMOPT_SOCKETDATA, this);
        manager_setopt(handler, CURLMOPT_TIMERFUNCTION, &curl_timer_cb);
        manager_setopt(handler, CURLMOPT_TIMERDATA, this);
    }

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    ~Manager()
    {
        connections_.clear();

        if (handler)
        {
            curl_multi_cleanup(handler);
        }
    }

    template <typename T>
    static void manager_setopt(CURLM* handle, CURLMoption opt, T t)
    {
        auto rc = curl_multi_setopt(handle, opt, t);
        if (rc != CURLM_OK)
        {
            BOOST_LOG_TRIVIAL(error)
                << "Error setting curl option: " << curl_multi_strerror(rc);
            throw std::runtime_error("Cannot set option on curl");
        }
    }

    void timer_cb(const boost::system::error_code& error)
    {
        if (!error)
        {
            int still_running = 0;
            auto rc = curl_multi_socket_action(
                handler, CURL_SOCKET_TIMEOUT, 0, &still_running);

            if (rc != CURLM_OK)
            {
                BOOST_LOG_TRIVIAL(error)
                    << "Error curl multi: " << curl_multi_strerror(rc);
                throw std::runtime_error("timer_cb error");
            }

            checkHandles();
        }
    }

    static int curl_timer_cb(CURLM*, long timeout_ms, void* userdata)
    {
        Manager* manager = (Manager*)userdata;

        manager->timer.cancel();

        if (timeout_ms > 0)
        {
            manager->timer.expires_from_now(boost::posix_time::millisec(timeout_ms));
            manager->timer.async_wait(
                std::bind(&Manager::timer_cb, manager, std::placeholders::_1));
        }
        else
        {
            boost::system::error_code error; /*success*/
            manager->timer_cb(error);
        }

        return 0;
    }

    static int sock_cb(CURL* easy,
                       curl_socket_t s,
                       int what,
                       void* multi_private,
                       void* socket_private)
    {
        Manager* manager = (Manager*)multi_private;

        if (what == CURL_POLL_REMOVE)
        {
            return 0;
        }

        if (!socket_private)
        {
            void* v = nullptr;
            auto rc = curl_easy_getinfo(easy, CURLINFO_PRIVATE, &v);
            if (rc != CURLE_OK)
            {
                throw std::runtime_error("Cannot get private info:" +
                                         std::string(curl_easy_strerror(rc)));
            }

            curl_multi_assign(manager->handler, s, v);
            socket_private = v;
        }

        Connection* connection = (Connection*)socket_private;
        manager->poll(connection, what);
        return 0;
    }

    void checkHandles()
    {
        CURLMsg* msg;
        int msgs_left = 0;
        while ((msg = curl_multi_info_read(handler, &msgs_left)))
        {
            if (msg->msg == CURLMSG_DONE)
            {
                auto easy = msg->easy_handle;
                auto result = msg->data.result;

                Connection* conn;
                curl_easy_getinfo(easy, CURLINFO_PRIVATE, &conn);
                auto rc = curl_multi_remove_handle(handler, easy);

                if (rc != CURLM_OK)
                {
                    BOOST_LOG_TRIVIAL(error) << "Cannot unregister easy handle";
                }

                pool.post([result, conn]
                          { conn->buildResponse(result); });
            }
        }
    }

    void performOp(Connection* connection, int action)
    {
        int still_running = 0;
        auto rc = curl_multi_socket_action(
            handler, connection->socket.native_handle(), action, &still_running);
        if (rc != CURLM_OK)
        {
            throw std::runtime_error(curl_multi_strerror(rc));
        }

        checkHandles();

        if (still_running <= 0)
        {
            timer.cancel();
        }

    }

    void poll(Connection* connection, int action)
    {
        boost::asio::ip::tcp::socket& tcp_socket = connection->socket;

        if (action == CURL_POLL_IN)
        {
            tcp_socket.async_read_some(
                boost::asio::null_buffers(),
                strand.wrap(
                    std::bind(&Manager::performOp, this, connection, action)));
        }
        else if (action == CURL_POLL_OUT)
        {
            tcp_socket.async_write_some(
                boost::asio::null_buffers(),
                strand.wrap(
                    std::bind(&Manager::performOp, this, connection, action)));
        }
        else if (action == CURL_POLL_INOUT)
        {
            tcp_socket.async_read_some(
                boost::asio::null_buffers(),
                strand.wrap(
                    std::bind(&Manager::performOp, this, connection, action)));

            tcp_socket.async_write_some(
                boost::asio::null_buffers(),
                strand.wrap(
                    std::bind(&Manager::performOp, this, connection, action)));
        }
    }

    void handleRequest(Method method, Connection::ConnectionPtr connection)
    {

        Connection* conn = connection.release();

        strand.post([this, method, conn]()
                    {
                        try
                        {
                            conn->configureRequest(method);
                        }
                        catch (...)
                        {
                            conn->promise.set_exception(std::current_exception());
                        }

                        auto rc = curl_multi_add_handle(handler, conn->handle);
                        if (rc != CURLM_OK)
                        {
                            std::string message = curl_multi_strerror(rc);
                            BOOST_LOG_TRIVIAL(error)
                                << "Error scheduling a new request: " << message;
                            conn->promise.set_exception(std::make_exception_ptr(
                                std::runtime_error(message)));
                        }
                    });
    }

    CURLM* handler;
    UTILS::ThreadPool& pool;
    UTILS::ThreadPool::Strand strand;
    UTILS::ThreadPool::Timer timer;

    std::vector<Connection::ConnectionPtr> connections_;
};

HttpClient::HttpClient(size_t nb_thread)
: pool_(nb_thread, service_)
, manager(new Manager(pool_))
{
    std::call_once(curl_init_flag, &init_curl);
    pool_.start();
}

HttpClient::~HttpClient()
{
    pool_.stop();
}

HttpClient::Future HttpClient::handleRequest(HTTP::Method method, Request&& request)
{
    Connection::ConnectionPtr connection;

    if (request.connection_)
    {
        connection = std::move(request.connection_);
    }
    else
    {
        connection = Connection::createConnection(service_);
    }

    connection->init();
    connection->request = std::move(request);
    auto future = connection->promise.get_future();

    manager->handleRequest(method, std::move(connection));
    return future;
}

#define METHOD_post POST
#define METHOD_get GET
#define METHOD_head HEAD
#define METHOD_put PUT
#define METHOD_delete_ DELETE_
#define METHOD_options OPTIONS
#define METHOD_trace TRACE
#define METHOD_connect CONNECT

#define METHOD(m)                                                       \
    HttpClient::Future HttpClient::async_##m(Request&& req)             \
    {                                                                   \
        return handleRequest(HTTP::Method::METHOD_##m, std::move(req)); \
    }                                                                   \
    HttpClient::Response HttpClient::m(Request&& request)               \
    {                                                                   \
        return async_##m(std::move(request)).get();                     \
    }

METHOD(post);
METHOD(get);
METHOD(head);
METHOD(put);
METHOD(delete_);
METHOD(options);
METHOD(trace);
METHOD(connect);

} // namespace HTTPP
