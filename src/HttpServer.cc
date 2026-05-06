#include "HttpServer.h"

#include "Buffer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"

HttpServer::HttpServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
    : server_(loop, addr, name)
    , loop_(loop)
{
    server_.setConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));

    server_.setMessageCallback(
        std::bind(&HttpServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

void HttpServer::onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected())
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        contexts_.emplace(conn->name(), std::make_shared<HttpContext>());
    }
    else
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        contexts_.erase(conn->name());
    }
}

void HttpServer::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    std::shared_ptr<HttpContext> contextPtr;
    {
        std::lock_guard<std::mutex> lock(contextsMutex_);
        auto it = contexts_.find(conn->name());
        if (it == contexts_.end())
        {
            auto created = std::make_shared<HttpContext>();
            contexts_.emplace(conn->name(), created);
            contextPtr = created;
        }
        else
        {
            contextPtr = it->second;
        }
    }
    HttpContext &context = *contextPtr;
    bool badRequest = false;

    while (context.parseRequest(buf, &badRequest))
    {
        HttpResponse response;
        HttpRequest &request = context.request();
        response.setCloseConnection(!request.keepAlive());

        if (!router_.route(request, &response))
        {
            response.setStatus(404, "Not Found");
            response.setHeader("Content-Type", "text/plain");
            response.setBody("Not Found");
        }

        conn->send(response.toString());
        if (response.closeConnection())
        {
            conn->shutdown();
            context.reset();
            break;
        }
        context.reset();
    }

    if (badRequest)
    {
        HttpResponse response;
        response.setStatus(400, "Bad Request");
        response.setHeader("Content-Type", "text/plain");
        response.setBody("Bad Request");
        response.setCloseConnection(true);
        conn->send(response.toString());
        conn->shutdown();
        context.reset();
    }
}
