#include "CoHttpServer.h"

#include <cstring>

#include "Buffer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "TcpConnection.h"

#include <co_routine.h>

CoHttpServer::CoHttpServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
    : server_(loop, addr, name)
    , loop_(loop)
{
    server_.setConnectionCallback(
        std::bind(&CoHttpServer::onConnection, this, std::placeholders::_1));

    server_.setMessageCallback(
        std::bind(&CoHttpServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

void CoHttpServer::onConnection(const TcpConnectionPtr &conn)
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

void CoHttpServer::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
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

    // 解析出一个完整的 HTTP 请求后，启动协程处理
    if (context.parseRequest(buf, &badRequest))
    {
        CoHandlerArg *arg = new CoHandlerArg();
        arg->router = &router_;
        arg->conn = conn;
        arg->request = context.request();
        arg->response.setCloseConnection(!arg->request.keepAlive());

        // 创建并启动协程，协程内部调用 handler 并通过 sendCo 发送响应
        stCoRoutineAttr_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.stack_size = 128 * 1024; // 128KB 私有栈
        attr.share_stack = NULL;

        stCoRoutine_t *co = NULL;
        co_create(&co, &attr, coHandlerRoutine, arg);
        co_resume(co); // 启动协程；若 handler 无 I/O 等待则同步完成，否则 yield

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

// 协程入口函数：在 libco 协程上下文中运行 HTTP handler
void *CoHttpServer::coHandlerRoutine(void *arg)
{
    CoHandlerArg *ha = static_cast<CoHandlerArg *>(arg);

    // 启用系统调用 hook，使 read/write/send/recv 等阻塞调用自动 yield 协程
    co_enable_hook_sys();

    // 路由并执行 handler（handler 在此协程上下文中运行）
    if (!ha->router->route(ha->request, &ha->response))
    {
        ha->response.setStatus(404, "Not Found");
        ha->response.setHeader("Content-Type", "text/plain");
        ha->response.setBody("Not Found");
    }

    // 通过 sendCo 发送响应；若内核发送缓冲区满，sendCo 会 yield 协程等待 POLLOUT
    ha->conn->sendCo(ha->response.toString());

    if (ha->response.closeConnection())
    {
        ha->conn->shutdown();
    }

    delete ha;
    return NULL;
}
