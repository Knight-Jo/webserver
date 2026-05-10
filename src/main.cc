#include <string>
#include <cstring>
#include <vector>

#include <TcpServer.h>
#include <CoHttpServer.h>
#include <HttpRequest.h>
#include <HttpResponse.h>
#include <Logger.h>
#include <sys/stat.h>
#include <sstream>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

// 阻塞 I/O 演示 —— libco hook 所需头文件
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#include "AsyncLogging.h"
#include "LFU.h"
#include "memoryPool.h"
#if defined(USE_GPERFTOOLS)
#include <gperftools/profiler.h>
#endif
// 日志文件滚动大小为1MB (1*1024*1024字节)
static const off_t kRollSize = 1*1024*1024;
AsyncLogging* g_asyncLog = NULL;
AsyncLogging * getAsyncLog(){
    return g_asyncLog;
}
 void asyncLog(const char* msg, int len)
{
    AsyncLogging* logging = getAsyncLog();
    if (logging)
    {
        logging->append(msg, len);
    }
}
int main(int argc,char *argv[]) {
#if defined(USE_GPERFTOOLS)
    const char *profilePath = "./my_profile.prof";
        // 注册信号处理，确保在打死时也能正常停止
    ProfilerStart(profilePath);
    signal(SIGINT, [](int) {
        ProfilerStop();
        exit(0);
    });
    signal(SIGTERM, [](int) {
        ProfilerStop();
        exit(0);
    });
    std::atexit(ProfilerStop);
#endif
    //第一步启动日志，双缓冲异步写入磁盘.
    //创建一个文件夹
    const std::string LogDir="logs";
    mkdir(LogDir.c_str(),0755);
    //使用std::stringstream 构建日志文件夹
    std::ostringstream LogfilePath;
    LogfilePath << LogDir << "/" << ::basename(argv[0]); // 完整的日志文件路径
    AsyncLogging log(LogfilePath.str(), kRollSize);
    g_asyncLog = &log;
    Logger::setOutput(asyncLog); // 为Logger设置输出回调, 重新配接输出位置
    log.start(); // 开启日志后端线程
    //第二步启动内存池和LFU缓存
     // 初始化内存池
    memoryPool::HashBucket::initMemoryPool();

    // 初始化缓存
    const int CAPACITY = 5;  
    KamaCache::KLfuCache<int, std::string> lfu(CAPACITY);
    //第三步启动底层网络模块
    EventLoop loop;
    InetAddress addr(8080, "0.0.0.0");
    CoHttpServer server(&loop, addr, "CoHttpServer");
    server.setThreadNum(16);
    server.addRoute("GET", "/", [](const HttpRequest &, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        response->setBody("Hello, world");
    });
    server.addRoute("POST", "/echo", [](const HttpRequest &request, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        response->setBody(request.body());
    });
    // 协程示例：读取本地文件并返回（在协程中自动 yield/恢复）
    server.addRoute("GET", "/file", [](const HttpRequest &, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        int fd = ::open("/proc/self/status", O_RDONLY);
        if (fd < 0)
        {
            response->setBody("open failed");
            return;
        }
        // 在协程中使用 hooked read() —— 若 fd 不可读则自动 yield
        char buf[4096];
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0)
        {
            response->setBody(std::string(buf, n));
        }
        ::close(fd);
    });

    // ========== 阻塞 I/O 演示路由 ==========

    // GET /resolve?host=example.com
    // DNS 解析（非阻塞，协程 yield）
    server.addRoute("GET", "/resolve", [](const HttpRequest &req, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");

        auto it = req.queryParams().find("host");
        std::string host = (it != req.queryParams().end()) ? it->second : "localhost";

        struct addrinfo hints, *res = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int ret = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (ret != 0 || !res)
        {
            response->setBody("DNS resolution failed for: " + host + " (" + ::gai_strerror(ret) + ")");
            return;
        }

        std::string result = "Host: " + host + "\nIP addresses:\n";
        char ip[64];
        for (struct addrinfo *rp = res; rp != nullptr; rp = rp->ai_next)
        {
            struct sockaddr_in *sin = (struct sockaddr_in *)rp->ai_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            result += "  " + std::string(ip) + "\n";
        }
        ::freeaddrinfo(res);

        response->setBody(result);
    });

    // GET /fetch?host=HOST&port=80&path=/
    // 在协程内部创建 TCP 连接并发送 HTTP 请求
    // 演示 hooked: getaddrinfo → socket → connect → send → recv → close
    server.addRoute("GET", "/fetch", [](const HttpRequest &req, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");

        // 解析参数
        auto getParam = [&](const std::string &key, const std::string &def) -> std::string {
            auto it = req.queryParams().find(key);
            return (it != req.queryParams().end()) ? it->second : def;
        };
        std::string host = getParam("host", "httpbin.org");
        int port = std::stoi(getParam("port", "80"));
        std::string path = getParam("path", "/get");

        // 1. DNS 解析（非阻塞，协程 yield）
        struct addrinfo hints, *res = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int ret = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (ret != 0 || !res)
        {
            response->setBody("DNS resolution failed: " + host);
            return;
        }

        // 2. 创建 socket —— 被 libco hook，注册到 hook 系统
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            ::freeaddrinfo(res);
            response->setBody("socket() failed");
            return;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        memcpy(&addr.sin_addr, &sin->sin_addr, sizeof(sin->sin_addr));
        ::freeaddrinfo(res);

        // 3. connect —— 被 libco hook
        // 非阻塞 socket + EINPROGRESS → 进入 poll() 等待 → 协程 yield
        // 直到 TCP 握手完成或超时（默认 75s）
        if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            ::close(sock);
            response->setBody("connect() failed to " + host + ":" + std::to_string(port));
            return;
        }

        // 4. 构建并发送 HTTP GET 请求
        std::string httpReq = "GET " + path + " HTTP/1.0\r\n"
                              "Host: " + host + "\r\n"
                              "Connection: close\r\n"
                              "\r\n";

        // send —— 被 libco hook，若发送缓冲区满则 yield 等待 POLLOUT
        size_t sent = 0;
        while (sent < httpReq.size())
        {
            ssize_t n = ::send(sock, httpReq.data() + sent, httpReq.size() - sent, 0);
            if (n <= 0) break;
            sent += n;
        }

        if (sent < httpReq.size())
        {
            ::close(sock);
            response->setBody("send() failed after " + std::to_string(sent) + " bytes");
            return;
        }

        // 5. 读取 HTTP 响应 —— recv 被 libco hook
        // 若对端尚未发送数据，协程 yield 等待 POLLIN
        std::string respBody;
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0)
        {
            respBody.append(buf, n);
        }

        ::close(sock);

        // 解析 HTTP 响应，提取 body（简单实现：以 \r\n\r\n 分割）
        auto pos = respBody.find("\r\n\r\n");
        if (pos != std::string::npos)
        {
            std::string headerPart = respBody.substr(0, pos);
            std::string bodyPart = respBody.substr(pos + 4);

            // 尝试在 header 中查找 Content-Length
            auto clPos = headerPart.find("Content-Length: ");
            if (clPos != std::string::npos)
            {
                auto clEnd = headerPart.find("\r\n", clPos);
                int contentLen = std::stoi(headerPart.substr(clPos + 16, clEnd - clPos - 16));
                bodyPart = bodyPart.substr(0, contentLen);
            }

            response->setBody(bodyPart);
        }
        else
        {
            response->setBody(respBody);
        }
    });

    // GET /pingback?host=HOST&port=PORT
    // 连接 -> 发送一个简单请求 -> 读取响应，演示完整的 hooked I/O 流水线
    server.addRoute("GET", "/pingback", [](const HttpRequest &req, HttpResponse *response) {
        auto getParam = [&](const std::string &key, const std::string &def) -> std::string {
            auto it = req.queryParams().find(key);
            return (it != req.queryParams().end()) ? it->second : def;
        };
        std::string host = getParam("host", "127.0.0.1");
        int port = std::stoi(getParam("port", "8080"));

        // 1. DNS（非阻塞，协程 yield）
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        struct addrinfo hints, *res = nullptr;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int ret = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (ret != 0 || !res) { response->setBody("DNS failed"); return; }
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        memcpy(&addr.sin_addr, &sin->sin_addr, sizeof(sin->sin_addr));
        ::freeaddrinfo(res);

        // 2. socket
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { response->setBody("socket failed"); return; }

        // 3. connect (hook → yield)
        if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        {
            ::close(sock);
            response->setBody("connect failed");
            return;
        }

        // 4. send (hook → yield 如果发送缓冲区满)
        const std::string httpReq = "GET / HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < httpReq.size())
        {
            ssize_t n = ::send(sock, httpReq.data() + sent, httpReq.size() - sent, 0);
            if (n <= 0) break;
            sent += n;
        }
        if (sent < httpReq.size()) { ::close(sock); response->setBody("send failed"); return; }

        // 5. recv (hook → yield 等待响应数据)
        std::string raw;
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0)
        {
            raw.append(buf, n);
        }
        ::close(sock);

        std::string result;
        result += "request: GET / HTTP/1.0\r\n";
        result += "response length: " + std::to_string(raw.size()) + " bytes\n";
        result += "response body:\n";

        auto pos = raw.find("\r\n\r\n");
        if (pos != std::string::npos)
            result += raw.substr(pos + 4);
        else
            result += raw;

        response->setBody(result);
    });
    server.start();
 // 主loop开始事件循环  epoll_wait阻塞 等待就绪事件(主loop只注册了监听套接字的fd，所以只会处理新连接事件)
    std::cout << "================================================Start Web Server================================================" << std::endl;
    loop.loop();
    std::cout << "================================================Stop Web Server=================================================" << std::endl;
    //结束日志打印
    log.stop();
}
