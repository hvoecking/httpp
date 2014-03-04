/*
 * Part of HTTPP.
 *
 * Distributed under the 3-clause BSD licence (See LICENCE.TXT file at the
 * project root).
 *
 * Copyright (c) 2014 Thomas Sanchez.  All rights reserved.
 *
 */

#include "Connection.hpp"

#include <curl/curl.h>
#include <memory>
#include <iostream>
#include <vector>

#include <boost/thread/future.hpp>
#include <boost/log/trivial.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/exception_ptr.hpp>

#include "httpp/http/Protocol.hpp"
#include "httpp/http/client/Request.hpp"
#include "httpp/http/client/Response.hpp"
#include "httpp/utils/Exception.hpp"

namespace HTTPP {
namespace HTTP {
namespace client {

namespace detail {

Connection::Connection(boost::asio::io_service& service)
: handle(curl_easy_init())
, service(service)
{
    if (!handle)
    {
        throw std::runtime_error("Cannot initialize curl handle");
    }
}

Connection::~Connection()
{
    if (http_headers)
    {
        curl_slist_free_all(http_headers);
    }

    if (handle)
    {
        curl_easy_cleanup(handle);
    }

    if (!is_canceled && !result_notified)
    {
        BOOST_LOG_TRIVIAL(error) << "Destroy a not completed connection";
        complete(std::make_exception_ptr(
            std::runtime_error("Destroy a non completed connection")));
    }

    if (is_canceled)
    {
        cancel_promise.set_value();
    }
}

void Connection::init(
    std::map<curl_socket_t, boost::asio::ip::tcp::socket*>& sockets)

{

    if (!result_notified)
    {
        throw std::runtime_error("Recycle a not completed connection");
    }

    curl_easy_reset(handle);

    this->sockets = std::addressof(sockets);

    conn_setopt(CURLOPT_HEADERFUNCTION, &writeHd);
    conn_setopt(CURLOPT_WRITEHEADER, this);

    conn_setopt(CURLOPT_WRITEFUNCTION, &writefn);
    conn_setopt(CURLOPT_WRITEDATA, this);

    conn_setopt(CURLOPT_ERRORBUFFER, this->error_buffer);

    conn_setopt(CURLOPT_PRIVATE, this);

    conn_setopt(CURLOPT_OPENSOCKETDATA, this);
    conn_setopt(CURLOPT_OPENSOCKETFUNCTION, &opensocket);

    conn_setopt(CURLOPT_CLOSESOCKETDATA, this->sockets);
    conn_setopt(CURLOPT_CLOSESOCKETFUNCTION, &closesocket);

    conn_setopt(CURLOPT_ERRORBUFFER, this->error_buffer);
    conn_setopt(CURLOPT_PRIVATE, this);

    if (http_headers)
    {
        curl_slist_free_all(http_headers);
        http_headers = nullptr;
    }

    expect_continue = false;
    header.clear();
    buffer.clear();

    promise = boost::promise<client::Response>();
    cancel_promise = boost::promise<void>();
    completion_handler = CompletionHandler();
    response = Response();
    is_polling = false;
    is_canceled = false;
    poll_action = 0;
}

Connection::ConnectionPtr
Connection::createConnection(boost::asio::io_service& service)
{
    return std::make_shared<Connection>(service);
}

size_t Connection::writeHd(char* buffer, size_t size, size_t nmemb, void* userdata)
{
    Connection* conn = (Connection*)userdata;
    auto actual_size = size * nmemb;
    conn->header.insert(std::end(conn->header), buffer, buffer + actual_size);
    return actual_size;
}

size_t Connection::writefn(char* buffer, size_t size, size_t nmemb, void* userdata)
{
    Connection* conn = (Connection*)userdata;

    auto actual_size = size * nmemb;
    conn->buffer.insert(std::end(conn->buffer), buffer, buffer + actual_size);
    return actual_size;
}

curl_socket_t Connection::opensocket(void* clientp,
                                     curlsocktype purpose,
                                     struct curl_sockaddr* address)
{
    Connection* conn = (Connection*)clientp;

    if (purpose == CURLSOCKTYPE_IPCXN)
    {
        boost::system::error_code ec;
        boost::asio::ip::tcp::socket* socket =
            new boost::asio::ip::tcp::socket(conn->service);

        if (address->family == AF_INET)
        {
            socket->open(boost::asio::ip::tcp::v4(), ec);
        }
        else
        {
            socket->open(boost::asio::ip::tcp::v6(), ec);
        }

        if (ec)
        {
            BOOST_LOG_TRIVIAL(error)
                << "Cannot open a socket: " << ec.message();
            return CURL_SOCKET_BAD;
        }

        auto handle = socket->native_handle();
        (*conn->sockets)[handle] = socket;
        return handle;
    }

    return CURL_SOCKET_BAD;
}

int Connection::closesocket(void* clientp, curl_socket_t curl_socket)
{
    std::map<curl_socket_t, boost::asio::ip::tcp::socket*>* sockets;
    sockets = (decltype(sockets)) clientp;

    auto it = sockets->find(curl_socket);
    if (it == std::end(*sockets))
    {
        BOOST_LOG_TRIVIAL(error) << "Cannot find a socket to close";
        return 1;
    }

    delete it->second;
    sockets->erase(it);
    return 0;
}

void Connection::setSocket(curl_socket_t curl_socket)
{
    auto it = sockets->find(curl_socket);
    if (it == std::end(*sockets))
    {
        throw std::runtime_error("Cannot find socket: " +
                                 std::to_string(curl_socket));
    }

    socket = it->second;
}

void Connection::configureRequest(HTTPP::HTTP::Method method)
{

    switch (method)
    {
     case HTTPP::HTTP::Method::GET:
        conn_setopt(CURLOPT_HTTPGET, 1L);
        break;
     case HTTPP::HTTP::Method::POST:
        conn_setopt(CURLOPT_POST, 1L);
        break;
     case HTTPP::HTTP::Method::HEAD:
        conn_setopt(CURLOPT_NOBODY, 1L);
        break;

     case HTTPP::HTTP::Method::PUT:
     case HTTPP::HTTP::Method::DELETE_:
     case HTTPP::HTTP::Method::OPTIONS:
     case HTTPP::HTTP::Method::TRACE:
     case HTTPP::HTTP::Method::CONNECT:
         std::string method_str = to_string(method);
         conn_setopt(CURLOPT_CUSTOMREQUEST, method_str.data());
         break;
    }

    if (!request.query_params_.empty())
    {
        std::string url = request.url_ + "?";

        bool add_sep = false;

        for (const auto& query_param : request.query_params_)
        {
            if (add_sep)
            {
                url += "&";
            }
            url += query_param.first + "=" + query_param.second;
            add_sep = true;
        }

        conn_setopt(CURLOPT_URL, url.data());
    }
    else
    {
        conn_setopt(CURLOPT_URL, request.url_.data());
    }

    if (request.follow_redirect_)
    {
        conn_setopt(CURLOPT_FOLLOWLOCATION, 1L);
        conn_setopt(CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
    }

    if (!request.post_params_.empty() && !request.content_.empty())
    {
        throw std::runtime_error(
            "Cannot mix multipart and x-formurl-encoded post data");
    }

    struct curl_httppost* post = NULL;
    struct curl_httppost* last = NULL;
    if (!request.post_params_.empty())
    {
        for (const auto& e : request.post_params_)
        {
            curl_formadd(&post,
                            &last,
                            CURLFORM_COPYNAME,
                            e.first.data(),
                            CURLFORM_COPYCONTENTS,
                            e.second.data(),
                            CURLFORM_END);
        }

        conn_setopt(CURLOPT_HTTPPOST, post);
        expect_continue = true;
    }

    if (!request.content_.empty())
    {
        conn_setopt(CURLOPT_POSTFIELDS, request.content_.data());
        conn_setopt(CURLOPT_POSTFIELDSIZE, request.content_.size());
    }

    conn_setopt(CURLOPT_NOSIGNAL, 1L);

    // only on curl > 7.25, disable it for now
    //conn_setopt(CURLOPT_TCP_KEEPALIVE, 1L);

    if (!request.http_headers_.empty())
    {
        http_headers = nullptr;
        for (const auto& h : request.http_headers_)
        {
            http_headers = curl_slist_append(
                http_headers, (h.first + ": " + h.second).data());
        }

        conn_setopt(CURLOPT_HTTPHEADER, http_headers);
    }

    result_notified = false;
}

void Connection::cancel()
{
    BOOST_LOG_TRIVIAL(debug) << "Cancel operation on connection";
    is_canceled = true;

    if (is_polling)
    {
        boost::system::error_code ec;
        socket->cancel(ec);
    }
}

void Connection::buildResponse(CURLcode code)
{
    if (is_canceled)
    {
        BOOST_LOG_TRIVIAL(debug)
            << "Operation cancelled detected, skip response building";

        complete(std::make_exception_ptr(HTTPP::UTILS::OperationAborted()));
        return;
    }

    if (code != CURLE_OK)
    {
        complete(std::make_exception_ptr(std::runtime_error(
            curl_easy_strerror(code) + std::string(this->error_buffer))));
        return;
    }
    try
    {
        response.code = static_cast<HTTPP::HTTP::HttpCode>(
            conn_getinfo<long>(CURLINFO_RESPONSE_CODE));

        request.connection_ = shared_from_this();
        response.request = std::move(request);

        long redirection = 0;
        if (request.follow_redirect_)
        {
            redirection = conn_getinfo<long>(CURLINFO_REDIRECT_COUNT);
        }

        redirection += expect_continue ? 1 : 0;
        while (redirection)
        {
            auto begin = std::begin(header);
            auto end = std::end(header);
            auto it = std::search(begin,
                                  end,
                                  std::begin(HEADER_BODY_SEP),
                                  std::end(HEADER_BODY_SEP));

            if (it != end)
            {
                header.erase(std::begin(header), it + 4);
            }

            --redirection;
        }

        response.body.swap(buffer);
        parseCurlResponseHeader(header, response);
        complete();
    }
    catch (...)
    {
        complete(std::current_exception());
        return;
    }

}

void Connection::complete(std::exception_ptr ex)
{
    if (result_notified)
    {
        BOOST_LOG_TRIVIAL(error)
            << "Response already notified, cancel notification";
        return;
    }

    result_notified = true;

    if (ex)
    {
        try
        {
            std::rethrow_exception(ex);
        }
        catch (...)
        {
            promise.set_exception(boost::current_exception());
        }
    }
    else
    {
        promise.set_value(std::move(response));
    }

    if (completion_handler)
    {
        completion_handler(promise.get_future());
    }
}

} // namespace detail
} // namespace client
} // namespace HTTP
} // namespace HTTPP
