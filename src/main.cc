#include <string>

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
    InetAddress addr(8080);
    HttpServer server(&loop, addr, "HttpServer");
    server.setThreadNum(3);
    server.addRoute("GET", "/", [](const HttpRequest &, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        response->setBody("Hello, world");
    });
    server.addRoute("POST", "/echo", [](const HttpRequest &request, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        response->setBody(request.body());
    });
    server.start();
 // 主loop开始事件循环  epoll_wait阻塞 等待就绪事件(主loop只注册了监听套接字的fd，所以只会处理新连接事件)
    std::cout << "================================================Start Web Server================================================" << std::endl;
    loop.loop();
    std::cout << "================================================Stop Web Server=================================================" << std::endl;
    //结束日志打印
    log.stop();
}