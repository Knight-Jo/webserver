#include <string>
#include <cstring>
#include <vector>
#include <cstdlib>

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>

#include <TcpServer.h>
#include <HttpServer.h>
#include <HttpRequest.h>
#include <HttpResponse.h>
#include <Logger.h>
#include <sys/stat.h>
#include <sstream>
#include <cstdlib>
#include <csignal>
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
    HttpServer server(&loop, addr, "HttpServer");
    server.setThreadNum(16);
    server.addRoute("GET", "/", [](const HttpRequest &, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        response->setBody("Hello, world");
    });
    server.addRoute("POST", "/echo", [](const HttpRequest &request, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        response->setBody(request.body());
    });

    // 阻塞 I/O 路由（无 libco — handler 直接阻塞 EventLoop 线程，用于对比测试）
    server.addRoute("GET", "/file", [](const HttpRequest &, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        int fd = ::open("/proc/self/status", O_RDONLY);
        if (fd < 0)
        {
            response->setBody("open failed");
            return;
        }
        char buf[4096];
        ssize_t n = ::read(fd, buf, sizeof(buf));  // 阻塞线程
        if (n > 0)
        {
            response->setBody(std::string(buf, n));
        }
        ::close(fd);
    });

    server.addRoute("GET", "/resolve", [](const HttpRequest &req, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        auto it = req.queryParams().find("host");
        std::string host = (it != req.queryParams().end()) ? it->second : "localhost";
        struct hostent *he = ::gethostbyname(host.c_str());  // 阻塞 — DNS 查询期间整个线程卡住
        if (!he) {
            response->setBody("DNS resolution failed for: " + host);
            return;
        }
        std::string result = "Host: " + host + "\nIP addresses:\n";
        char ip[64];
        for (int i = 0; he->h_addr_list[i]; ++i) {
            inet_ntop(he->h_addrtype, he->h_addr_list[i], ip, sizeof(ip));
            result += "  " + std::string(ip) + "\n";
        }
        response->setBody(result);
    });

    server.addRoute("GET", "/fetch", [](const HttpRequest &req, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        auto getParam = [&](const std::string &key, const std::string &def) -> std::string {
            auto it = req.queryParams().find(key);
            return (it != req.queryParams().end()) ? it->second : def;
        };
        std::string host = getParam("host", "httpbin.org");
        int port = std::stoi(getParam("port", "80"));
        std::string path = getParam("path", "/get");

        struct hostent *he = ::gethostbyname(host.c_str());  // 阻塞
        if (!he) { response->setBody("DNS resolution failed"); return; }

        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { response->setBody("socket() failed"); return; }

        // 设置 blocking socket
        int flags = ::fcntl(sock, F_GETFL, 0);
        ::fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {  // 阻塞
            ::close(sock);
            response->setBody("connect() failed"); return;
        }

        std::string httpReq = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < httpReq.size()) {
            ssize_t n = ::send(sock, httpReq.data() + sent, httpReq.size() - sent, 0);  // 阻塞
            if (n <= 0) break;
            sent += n;
        }
        if (sent < httpReq.size()) { ::close(sock); response->setBody("send() failed"); return; }

        std::string respBody;
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0) {  // 阻塞
            respBody.append(buf, n);
        }
        ::close(sock);

        auto pos = respBody.find("\r\n\r\n");
        if (pos != std::string::npos)
            response->setBody(respBody.substr(pos + 4));
        else
            response->setBody(respBody);
    });

    server.addRoute("GET", "/pingback", [](const HttpRequest &req, HttpResponse *response) {
        auto getParam = [&](const std::string &key, const std::string &def) -> std::string {
            auto it = req.queryParams().find(key);
            return (it != req.queryParams().end()) ? it->second : def;
        };
        std::string host = getParam("host", "127.0.0.1");
        int port = std::stoi(getParam("port", "8080"));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        struct hostent *he = ::gethostbyname(host.c_str());  // 阻塞
        if (!he) { response->setBody("DNS failed"); return; }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { response->setBody("socket failed"); return; }
        int flags = ::fcntl(sock, F_GETFL, 0);
        ::fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);

        if (::connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {  // 阻塞
            ::close(sock); response->setBody("connect failed"); return;
        }

        const std::string httpReq = "GET / HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < httpReq.size()) {
            ssize_t n = ::send(sock, httpReq.data() + sent, httpReq.size() - sent, 0);
            if (n <= 0) break;
            sent += n;
        }
        if (sent < httpReq.size()) { ::close(sock); response->setBody("send failed"); return; }

        std::string raw;
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0) {
            raw.append(buf, n);
        }
        ::close(sock);

        std::string result;
        result += "request: GET / HTTP/1.0\r\n";
        result += "response length: " + std::to_string(raw.size()) + " bytes\n";
        auto pos = raw.find("\r\n\r\n");
        if (pos != std::string::npos) result += raw.substr(pos + 4);
        else result += raw;
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