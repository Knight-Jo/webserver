# libco 集成指南

本文档记录了将 libco 协程库集成到 kama-webserver 的完整过程，包括设计思路、代码变更、构建系统修改以及踩坑记录。帮助新成员理解为什么需要协程、如何集成以及常见的陷阱。

## 目录

1. [背景与动机](#1-背景与动机)
2. [整体架构设计](#2-整体架构设计)
3. [构建系统变更](#3-构建系统变更)
4. [核心代码变更](#4-核心代码变更)
5. [踩坑记录](#5-踩坑记录)
6. [验证方法](#6-验证方法)
7. [快速上手](#7-快速上手)

---

## 1. 背景与动机

### Reactor 模式下 I/O 阻塞问题

kama-webserver 使用经典的 Reactor 模式："one loop per thread"。主 Reactor（mainLoop）负责 accept 新连接，通过轮询分发到多个 sub-Reactor（subLoop）处理 I/O：

```
mainLoop  accept 连接
    │
    ├─→ subLoop 1 [epoll_wait → 处理多个连接的读写]
    ├─→ subLoop 2 [epoll_wait → 处理多个连接的读写]
    └─→ subLoop N [epoll_wait → 处理多个连接的读写]
```

每个 subLoop 在单线程内处理多个连接。问题是：**当一个连接的处理函数内部执行阻塞 I/O 时，该 subLoop 上的所有连接都被阻塞**。

例如，如果某个 HTTP handler 需要读文件或查询上游服务：

```cpp
server.addRoute("GET", "/file", [](const HttpRequest &, HttpResponse *response) {
    int fd = ::open("/some/file", O_RDONLY);
    char buf[4096];
    ::read(fd, buf, sizeof(buf));  // 阻塞！同一线程上的其他连接都受影响
    // ...
});
```

### libco 协程方案

libco 解决了这个问题：通过 hook 系统调用（`read`、`write`、`poll`、`send`、`recv` 等），在阻塞调用发生时**透明地让出（yield）当前协程**，线程回到 EventLoop 继续处理其他就绪事件。当 I/O 就绪时，协程被自动恢复。

效果：**用同步代码的风格，实现异步执行的效率**。

---

## 2. 整体架构设计

### 分层架构

```
┌─────────────────────────────────────────────┐
│  CoHttpServer（协程 HTTP 服务器）              │
│  - 解析请求 → 创建协程 → co_resume 执行 handler  │
├─────────────────────────────────────────────┤
│  EventLoop（事件循环，集成 libco 调度）         │
│  - 每轮迭代末尾调用 co_schedule_tick()          │
├─────────────────────────────────────────────┤
│  TcpConnection::sendCo（协程安全发送）          │
│  - 内核缓冲区满时 co_poll + yield              │
├─────────────────────────────────────────────┤
│  libco（协程核心库）                            │
│  - 上下文切换、系统调用 hook、epoll 调度         │
└─────────────────────────────────────────────┘
```

### 协程调度流程（完整链路）

```
subLoop EventLoop::loop()
  │
  ├─ poller_->poll(kPollTimeMs, &activeChannels_)    // epoll_wait 等待 I/O 事件
  ├─ 处理 activeChannels_ 的 handleEvent()            // 分发 I/O 事件
  │    └─ TcpConnection::handleRead()
  │         └─ CoHttpServer::onMessage()
  │              ├─ 解析 HTTP 请求
  │              ├─ co_create(&co, &attr, coHandlerRoutine, arg)
  │              └─ co_resume(co)
  │                   └─ coHandlerRoutine()
  │                        ├─ co_enable_hook_sys()    // 开启系统调用 hook
  │                        ├─ router->route()          // 执行业务 handler
  │                        │    └─ handler 内部 read() → 被 libco hook
  │                        │         → epoll_ctl(libco_epfd, ADD)
  │                        │         → co_yield()      // 协程让出，回到线程
  │                        └─ conn->sendCo()           // 协程安全发送
  ├─ doPendingFunctors()                              // 执行 pending 回调
  └─ co_schedule_tick()                              // 驱动 libco 协程调度
       └─ co_eventloop_tick(co_get_epoll_ct())
            ├─ co_epoll_wait(libco_epfd, 0)           // 非阻塞查询 libco 的 epoll
            ├─ 处理就绪事件 → co_resume(等待的协程)
            └─ 处理超时 → co_resume(超时的协程)
               ↓
         恢复后的协程继续执行被 hook 拦截的点
```

### 关键设计决策

| 决策 | 方案 | 理由 |
|------|------|------|
| 每个 subLoop 一个 libco 环境 | `co_init_curr_thread_env()` 在 `EventLoopThread::threadFunc()` 调用 | 每个线程独立 epoll 实例，无需跨线程同步 |
| EventLoop 驱动 libco 调度 | `loop()` 末尾调用 `co_schedule_tick()` | 非阻塞调度，与现有事件循环协作 |
| 私有栈而非共享栈 | `attr.share_stack = NULL`, stack_size = 128KB | 实现简单，无共享栈回收复杂度 |
| 每个完整 HTTP 请求一个协程 | `onMessage` 解析完成后创建协程 | 协程内可完整处理请求生命周期 |
| `sendCo` 代替 `send` | 新增方法，原始 `send` 不受影响 | 兼容原有非协程代码路径 |
| **`kPollTimeMs` 改为 100ms** | 原值 10000ms → 100ms | **关键！** 否则协程调度延迟高达 10 秒 |

---

## 3. 构建系统变更

### 文件变更清单

```
CMakeLists.txt           # 根 CMakeLists：添加 libco 子目录、dl 链接
src/CMakeLists.txt       # 链接 colib_static 到 src_lib（--whole-archive）
tests/CMakeLists.txt     # 链接 colib_static 到测试可执行文件
libco/CMakeLists.txt     # 添加 co_comm.cpp、设置 -fPIC
```

### 根 CMakeLists.txt

```cmake
# 添加 libco 子目录（必须在 add_subdirectory(src) 之前）
add_subdirectory(libco)

# LIBS 中添加 dl（libco hook 机制需要 dlsym/RTLD_NEXT）
set(LIBS
    pthread
    dl
)

# 添加 libco 头文件目录供所有子项目使用
include_directories(${CMAKE_SOURCE_DIR}/libco)
```

### src/CMakeLists.txt

```cmake
# 链接静态库 colib_static 到共享库 src_lib
# 必须使用 --whole-archive，确保 colib 符号被全部导出到 libsrc_lib.so
target_link_libraries(src_lib -Wl,--whole-archive colib_static -Wl,--no-whole-archive ${LIBS})

# main 可执行文件直接链接 colib_static（冗余，但简化 test/benchmark 的链接）
target_link_libraries(main src_lib memory_lib log_lib colib_static ${LIBS})
```

### tests/CMakeLists.txt

```cmake
target_link_libraries(http_tests src_lib memory_lib log_lib colib_static ${LIBS})
```

### libco/CMakeLists.txt

```cmake
# 添加 co_comm.cpp（libco 内部同步原语，供 co_hook_sys_call.cpp 使用）
set(SOURCE_FILES
    ...
    co_comm.cpp)

# 强制 -fPIC：当 colib_static 被链接进共享库（libsrc_lib.so）时需要
set_target_properties(colib_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(colib_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)
```

### libco 本身的修改

在 `co_routine.h` 中添加了新函数声明：

```c
void co_eventloop_tick(stCoEpoll_t *ctx);
```

在 `co_routine.cpp` 中实现了该函数（从 `co_eventloop()` 中提取的非阻塞单轮调度版本）：

```cpp
void co_eventloop_tick(stCoEpoll_t *ctx)
{
    if (!ctx->result)
        ctx->result = co_epoll_res_alloc(stCoEpoll_t::_EPOLL_SIZE);

    co_epoll_res *result = ctx->result;
    int ret = co_epoll_wait(ctx->iEpollFd, result, stCoEpoll_t::_EPOLL_SIZE, 0);

    // 处理就绪事件：标记超时，恢复协程
    for (int i = 0; i < ret; i++)
    {
        struct epoll_event *ev = result->epoll_event + i;
        if (ev->events & EPOLLERR)  ev->events |= (EPOLLIN | EPOLLOUT);
        if (ev->events & EPOLLHUP)  ev->events |= (EPOLLIN | EPOLLOUT);

        stPollItem_t *item = (stPollItem_t *)ev->data.ptr;
        if ((ev->events & EPOLLOUT) && (item->pfnPrepare & 0x01)) { ... }
        if ((ev->events & EPOLLIN)  && !(item->pfnPrepare & 0x01)) { ... }

        OnPollProcessEvent(item);
    }

    // 处理超时
    stTimeoutItemLink_t *timeout_list = TakeAllTimeout(ctx->pTimeout);
    for (...)
    {
        item->bTimeout = true;
        OnPollProcessEvent(item);
    }

    // 恢复就绪/超时的协程
    while (!(result->lstActive.empty()))
    {
        stTimeoutItem_t *active = result->lstActive.front().data;
        result->lstActive.pop_front();
        if (active->pfnProcess)
            active->pfnProcess(active);
    }
}
```

---

## 4. 核心代码变更

### 4.1 新增文件

#### `include/CoHttpServer.h` — 协程 HTTP 服务器头文件

```cpp
class CoHttpServer
{
public:
    CoHttpServer(EventLoop *loop, const InetAddress &addr, const std::string &name);

    void setThreadNum(int numThreads) { server_.setThreadNum(numThreads); }
    void addRoute(const std::string &method, const std::string &path, const HttpHandler &handler)
    { router_.addRoute(method, path, handler); }
    void start() { server_.start(); }

private:
    void onConnection(const TcpConnectionPtr &conn);
    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time);

    struct CoHandlerArg
    {
        HttpRouter *router;
        HttpRequest request;
        HttpResponse response;
        TcpConnectionPtr conn;
    };
    static void *coHandlerRoutine(void *arg);

    TcpServer server_;
    EventLoop *loop_;
    HttpRouter router_;
    std::unordered_map<std::string, std::shared_ptr<HttpContext>> contexts_;
    std::mutex contextsMutex_;
};
```

设计说明：
- `CoHttpServer` 不是继承 HttpServer，而是**组合**（has-a）一个 `TcpServer` 实例。因为协程版本需要完全掌控消息处理和连接生命周期。
- 使用 `CoHandlerArg` 在堆上传递参数（因为协程执行时栈帧可能切换，不能使用栈上局部变量）。
- `contexts_` 使用 `std::shared_ptr<HttpContext>` 管理每个连接的 HTTP 解析状态，通过互斥锁保护。

#### `src/CoHttpServer.cc` — 协程 HTTP 服务器实现

核心逻辑在 `onMessage` 中：

```cpp
void CoHttpServer::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    // 获取该连接的 HttpContext（解析状态机）
    HttpContext &context = *contextPtr;
    bool badRequest = false;

    // 解析出一个完整 HTTP 请求后，创建协程
    if (context.parseRequest(buf, &badRequest))
    {
        CoHandlerArg *arg = new CoHandlerArg();
        arg->router = &router_;
        arg->conn = conn;
        arg->request = context.request();
        arg->response.setCloseConnection(!arg->request.keepAlive());

        // 创建协程（私有栈 128KB）
        stCoRoutineAttr_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.stack_size = 128 * 1024;
        attr.share_stack = NULL;

        stCoRoutine_t *co = NULL;
        co_create(&co, &attr, coHandlerRoutine, arg);
        co_resume(co);  // 启动协程

        context.reset();  // 重置解析状态机
    }
}
```

协程入口函数：

```cpp
void *CoHttpServer::coHandlerRoutine(void *arg)
{
    CoHandlerArg *ha = static_cast<CoHandlerArg *>(arg);

    co_enable_hook_sys();  // 开启系统调用 hook

    // 路由并执行业务 handler
    if (!ha->router->route(ha->request, &ha->response))
    {
        ha->response.setStatus(404, "Not Found");
        // ...
    }

    // 协程安全发送响应（内核缓冲区满时 yield）
    ha->conn->sendCo(ha->response.toString());

    if (ha->response.closeConnection())
        ha->conn->shutdown();

    delete ha;
    return NULL;
}
```

> **关键点**：`co_enable_hook_sys()` 必须在协程内调用，它设置线程局部变量 `co_sys_hook_enable_flag`，使 `dlsym(RTLD_NEXT)` 获取到的 hook 函数生效。

### 4.2 修改的文件

#### `src/EventLoop.cc` — 集成 libco 调度

```cpp
#include <co_routine.h>
#include <co_routine_inner.h>

void EventLoop::loop()
{
    looping_ = true;
    quit_ = false;

    while (!quit_)
    {
        activeChannels_.clear();
        pollRetureTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        for (Channel *channel : activeChannels_)
            channel->handleEvent(pollRetureTime_);

        doPendingFunctors();

        // 驱动 libco 协程调度（非阻塞，仅处理就绪事件）
        co_schedule_tick();
    }
}

> ⚠️ kPollTimeMs 原值为 10000ms（10 秒），改为 **100ms**。否则协程在 `recv()`/`send()` 等 hook 中 yield 后，即使 I/O 数据在微秒级到达，也要等待最长 10 秒才能被 `co_schedule_tick()` 恢复。

void EventLoop::co_schedule_tick()
{
    // 若当前线程未初始化 libco 环境，直接返回
    if (!co_get_curr_thread_env())
        return;

    // 非阻塞 poll + 处理就绪事件 + 处理超时 + 恢复协程
    co_eventloop_tick(co_get_epoll_ct());
}
```

#### `src/EventLoopThread.cc` — 初始化 libco 环境

```cpp
#include <co_routine.h>
#include <co_routine_inner.h>

void EventLoopThread::threadFunc()
{
    // 每个线程只需调用一次 libco 初始化
    co_init_curr_thread_env();

    EventLoop loop;
    // ...
    loop.loop();
}
```

#### `src/TcpConnection.cc` — 协程安全发送

```cpp
#include <co_routine.h>

void TcpConnection::sendCo(const std::string &buf)
{
    if (state_ != kConnected) return;

    size_t remaining = buf.size();
    const char *data = buf.data();

    // 尝试直接写入
    if (!channel_->isWriting())
    {
        ssize_t nwrote = ::write(channel_->fd(), data, remaining);
        if (nwrote >= 0)
        {
            if (static_cast<size_t>(nwrote) >= remaining)
                return;
            data += nwrote;
            remaining -= nwrote;
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK)
            return;
    }

    // 剩余数据等待 POLLOUT，使用 co_poll 让出协程
    while (remaining > 0 && state_ == kConnected)
    {
        struct pollfd pf;
        pf.fd = channel_->fd();
        pf.events = POLLOUT | POLLERR | POLLHUP;

        // co_poll 注册到 libco 的 epoll，然后 yield 当前协程
        // 当 fd 可写时，协程被恢复执行
        co_poll(co_get_epoll_ct(), &pf, 1, 1000);

        if (pf.revents & (POLLOUT | POLLERR | POLLHUP))
        {
            ssize_t nwrote = ::write(channel_->fd(), data, remaining);
            if (nwrote > 0)
            {
                data += nwrote;
                remaining -= nwrote;
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK)
                break;
        }
        else break; // 超时
    }
}
```

> **重要**：`sendCo()` 只能在协程上下文中调用（即 `coHandlerRoutine` 内部），因为 `co_poll` 需要 libco 的协程环境。

#### `src/main.cc` — 使用 CoHttpServer

```cpp
#include <CoHttpServer.h>

int main() {
    // ... 日志、内存池初始化 ...

    EventLoop loop;
    InetAddress addr(8080, "0.0.0.0");
    CoHttpServer server(&loop, addr, "CoHttpServer");
    server.setThreadNum(16);

    // 普通 handler（同步，无阻塞 I/O）
    server.addRoute("GET", "/", [](const HttpRequest &, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        response->setBody("Hello, world");
    });

    // 协程 handler：read() 会被 libco hook，自动 yield（演示用）
    server.addRoute("GET", "/file", [](const HttpRequest &, HttpResponse *response) {
        response->setHeader("Content-Type", "text/plain");
        int fd = ::open("/proc/self/status", O_RDONLY);
        if (fd < 0) { response->setBody("open failed"); return; }

        char buf[4096];
        ssize_t n = ::read(fd, buf, sizeof(buf));  // 被 hook，yield
        if (n > 0) response->setBody(std::string(buf, n));
        ::close(fd);
    });

    server.start();
    loop.loop();
}
```

---

## 5. 踩坑记录

### 坑 1：`extern "C"` 导致符号链接失败

**症状**：链接报 undefined reference，但 `nm libcolib.a` 显示符号确实存在。

```
/usr/bin/ld: ../../lib/libsrc_lib.so: undefined reference to `co_init_curr_thread_env'
```

**原因**：`co_routine.h` 是 C++ 头文件（包含 `stCoRoutineAttr_t` 构造函数等），但**不包含 `extern "C"` 声明**。libco 的 `.cpp` 文件编译出的是 C++ mangled 符号：

```cpp
// libco 编译出的符号（C++ mangled）
_Z23co_init_curr_thread_envv
_Z9co_createPP13stCoRoutine_tPK17stCoRoutineAttr_tPFPvS5_ES5_

// 我们的代码使用 extern "C" 后，期望的符号（unmangled）
co_init_curr_thread_env
co_create
```

**修复**：移除所有 `extern "C" { #include <co_routine.h> }` 包裹，直接 `#include <co_routine.h>`。

```cpp
// 错误 ❌
extern "C" {
#include <co_routine.h>
}

// 正确 ✓
#include <co_routine.h>
```

### 坑 2：TLS 重定位错误（R_X86_64_TPOFF32）

**症状**：

```
relocation R_X86_64_TPOFF32 against __tls_guard cannot be used when making shared object
```

**原因**：`co_hook_sys_call.cpp` 使用 `static __thread` 变量（TLS 初始执行模型），当 `libcolib_static.a` 被链接进共享库 `libsrc_lib.so` 时，`-ftls-model=initial-exec` 生成的 `R_X86_64_TPOFF32` 重定位在共享库中无效。

**修复**：在CMake中为 libco 目标启用 `POSITION_INDEPENDENT_CODE`：

```cmake
set_target_properties(colib_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(colib_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)
```

这会自动添加 `-fPIC` 编译选项，使 TLS 变量使用通用动态模型（general-dynamic），生成 `R_X86_64_TLSGD` 重定位，该类型在共享库中有效。

**注意**：使用 cmake 目标属性而非直接设置 `CMAKE_CXX_FLAGS`。`CMAKE_CXX_FLAGS` 覆盖 cmake 内部的默认 flags 可能导致编译器配置异常。

### 坑 3：`colib_static` 符号未导出到共享库

**症状**：即使正确设置了以上所有选项，链接可执行文件时仍报 undefined reference，libsrc_lib.so 中的 libco 符号无法解析。

**原因**：`libsrc_lib.so` 是共享库，GNU ld 默认在创建共享库时不会包含静态库中未被引用的所有符号。但是我们的 `src_lib` 源码确实引用了这些符号，所以一般情况下 ld 会包含它们。然而当问题出于其他原因（如签名不匹配）被掩盖时，需要使用 `--whole-archive` 强制导出。

**修复**：在链接 `src_lib` 时使用 `--whole-archive`：

```cmake
target_link_libraries(src_lib -Wl,--whole-archive colib_static -Wl,--no-whole-archive ${LIBS})
```

### 坑 4：`co_comm.cpp` 未编译

**症状**：链接报错 undefined reference to `clsCoMutex` 等符号。

**原因**：libco 的 `co_hook_sys_call.cpp` 使用 `co_comm.cpp` 中定义的 `clsCoMutex`，但原始 `libco/CMakeLists.txt` 的 `SOURCE_FILES` 中不包含 `co_comm.cpp`。

**修复**：将 `co_comm.cpp` 添加到 libco 的编译源文件列表中。

---

## 6. 验证方法

### 编译验证

```bash
cd build && cmake .. && make -j$(nproc)
# 应无错误输出，生成 bin/main、lib/libsrc_lib.so 等
```

### 功能验证

```bash
# 启动服务器
./bin/main &

# 测试普通路由
curl -s http://localhost:8080/
# 预期输出: Hello, world

# 测试协程 I/O handler（hooked read）
curl -s http://localhost:8080/file
# 预期输出: /proc/self/status 的内容

# 测试 404
curl -s http://localhost:8080/nonexistent
# 预期输出: Not Found

# 测试 POST
curl -s -X POST -d "hello echo" http://localhost:8080/echo
# 预期输出: hello echo

# 关闭服务器
kill %1
```

### 并发验证

可以使用 `wrk` 或 `ab` 进行压力测试：

```bash
# 安装 wrk
sudo apt install wrk

# 压力测试（16 线程子 Reactor）
wrk -t4 -c100 -d10s http://localhost:8080/
```

### 协程 yield 验证

在 handler 中加入耗时 I/O 操作，观察是否影响其他路由的响应。如果协程正常工作，长时间 I/O 的 handler 不会阻塞同线程上的其他请求。

---

## 7. 快速上手

### 新增一个协程 handler

```cpp
server.addRoute("GET", "/mydata", [](const HttpRequest &, HttpResponse *response) {
    response->setHeader("Content-Type", "application/json");

    // 在协程内阻塞读文件—不会阻塞线程
    int fd = ::open("/path/to/data.json", O_RDONLY);
    if (fd < 0) {
        response->setStatus(500, "Internal Server Error");
        response->setBody("{\"error\":\"open failed\"}");
        return;
    }
    char buf[65536];
    ssize_t n = ::read(fd, buf, sizeof(buf));
    ::close(fd);

    if (n > 0)
        response->setBody(std::string(buf, n));
    else
        response->setBody("{}");
});
```

### 注意事项

1. **`co_enable_hook_sys()` 必须在协程入口处调用**，否则系统调用不会被 hook。
2. **`sendCo()` 只在协程内使用**，协程外仍用 `send()`。
3. **协程参数用堆分配**：不要传递栈上局部变量的指针给协程，使用 `new` 创建参数结构体，协程结束时 `delete`。
4. **私有栈大小 128KB**：协程内不要定义巨大的栈上局部变量（如 `char buf[1024*1024]`），应使用堆或减小栈帧。
5. **线程安全**：libco 环境是线程局部的，无需跨线程同步（同一线程上的不同协程共享该线程的 epoll 实例）。
6. **已 hook 的系统调用**：`read`、`write`、`send`、`recv`、`sendto`、`recvfrom`、`connect`、`accept`、`poll`。不需要手动调用 `co_poll`，普通的 `read`/`write` 在 hook 开启后会自动 yield。
