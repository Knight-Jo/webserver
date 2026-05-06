#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "TcpServer.h"
#include "HttpRouter.h"
#include "HttpContext.h"

class HttpServer
{
public:
    HttpServer(EventLoop *loop, const InetAddress &addr, const std::string &name);

    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
    void addRoute(const std::string &method, const std::string &path, const HttpHandler &handler)
    { router_.addRoute(method, path, handler); }
    void start() { server_.start(); }

private:
    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time);

    TcpServer server_;
    EventLoop *loop_;
    HttpRouter router_;
    std::unordered_map<std::string, std::shared_ptr<HttpContext>> contexts_;
    std::mutex contextsMutex_;
};
