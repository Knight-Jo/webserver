# kama-webserver（libco 协程版）

基于 Reactor 模式的高性能 C++ Web 服务器，集成 libco 协程库实现阻塞 I/O 自动 yield，handler 可用同步代码风格实现异步执行效率。

**本项目在[知识星球](https://programmercarl.com/other/kstar.html)里维护，并答疑**

---

## 架构

### Reactor + 协程

经典「one loop per thread」Reactor 模型 + 每个 subReactor 集成 libco 协程调度：

```
subLoop EventLoop::loop()
  │
  ├─ poller_->poll(kPollTimeMs, &activeChannels_)
  ├─ 处理 activeChannels_ 分发 I/O 事件
  │    └─ TcpConnection::handleRead()
  │         └─ CoHttpServer::onMessage()
  │              ├─ 解析 HTTP 请求
  │              ├─ co_create() + co_resume(handlerRoutine)
  │              │    └─ co_enable_hook_sys()
  │              │    └─ router->route() 执行 handler
  │              │    │    └─ read() → libco hook → yield
  │              │    └─ conn->sendCo() 协程安全发送
  ├─ doPendingFunctors()
  └─ co_schedule_tick()     ← 驱动 libco 协程调度
       └─ co_epoll_wait(0)  ← 非阻塞，处理就绪+超时
            └─ co_resume(就绪协程)
```

### 协程调度流程

```
handler 内 read(fd, ...)        // 阻塞调用
  → libco hook 拦截
  → epoll_ctl(libco_epfd, ADD)  // 注册到 libco 的 epoll
  → co_yield()                  // 协程让出，线程回到 EventLoop
  → ... 线程处理其他就绪事件 ...
  → co_schedule_tick()
  → co_epoll_wait(libco_epfd)   // 发现 fd 就绪
  → co_resume(协程)             // 恢复执行
  → read() 返回数据
```

效果：**用同步代码的风格，实现异步执行的效率**。

### 核心模块

| 模块 | 说明 |
|------|------|
| **EventLoop** | 核心事件循环，集成 `co_schedule_tick()` 驱动协程调度 |
| **CoHttpServer** | 协程 HTTP 服务器，每个完整请求创建一个协程 |
| **EPollPoller** | epoll I/O 多路复用 |
| **TcpServer** | 用户态服务端 API，管理线程池和连接生命周期 |
| **TcpConnection::sendCo** | 协程安全发送，内核缓冲区满时 yield |
| **AsyncLogging** | 双缓冲异步日志 |
| **memoryPool** | 固定大小 slot 分配器 |
| **KLfuCache** | LFU 淘汰缓存 |

---

## 快速开始

### 构建

```bash
cd build && cmake .. && make -j$(nproc)
```

### 运行

```bash
./bin/main
# 服务监听 0.0.0.0:8080，16 工作线程
```

### 测试

```bash
# HTTP 功能测试
./build/http_tests

# 日志性能测试
./build/log_benchmark
./build/log_stress
./build/log_sync_vs_async

# 一键性能基准测试（含分支对比）
./benchmark.sh [branch_name] [server_binary]
./benchmark.sh compare   # 自动对比 main 和 feat/libco-integration
```

---

## API 路由

### GET `/` — 基础路由

```bash
curl http://localhost:8080/
# → Hello, world
```

### POST `/echo` — 回显

```bash
curl -X POST -d "hello" http://localhost:8080/echo
# → hello
```

### GET `/file` — 协程 hooked read

读取 `/proc/self/status`。`read()` 被 libco hook，fd 不可读时自动 yield。

```bash
curl http://localhost:8080/file
```

### GET `/resolve?host=<domain>` — 协程 DNS 解析

DNS 查询期间协程 yield，不阻塞线程。

```bash
curl "http://localhost:8080/resolve?host=example.com"
# → Host: example.com
#    IP addresses:
#      93.184.215.14
```

### GET `/fetch?host=<host>&port=<port>&path=<path>` — 协程反向代理

完整 hooked I/O 流水线：DNS → socket → connect → send HTTP GET → recv 响应。

```bash
curl "http://localhost:8080/fetch?host=example.com"
# → 返回 example.com 首页 HTML（约 510ms）
```

### GET `/pingback?host=<host>&port=<port>` — 协程自连接

连接回自身（8080），发送 GET / 请求并读取响应。全过程协程 yield。

```bash
curl "http://localhost:8080/pingback?host=127.0.0.1&port=8080"
# → request: GET / HTTP/1.0
#    response body:
#    Hello, world（约 207ms）
```

---

## libco 集成设计要点

| 设计决策 | 方案 |
|----------|------|
| 每个 subLoop 一个 libco 环境 | `co_init_curr_thread_env()` 在 `EventLoopThread::threadFunc()` 中调用 |
| 协程调度在 EventLoop 中 | `loop()` 末尾调用 `co_schedule_tick()`，非阻塞驱动 libco |
| 私有栈 128KB | 每个协程独立栈，无共享栈回收复杂度 |
| 每请求一协程 | `onMessage` 解析完成创建协程，一次 resume 运行到结束 |
| `sendCo` 代替 `send` | 仅在协程上下文中使用，内核缓冲区满时 `co_poll` yield |
| `kPollTimeMs = 100` | 原值 10000ms → 100ms，否则协程调度延迟高达 10 秒 |

### 被 libco hook 的系统调用

`read`、`write`、`send`、`recv`、`sendto`、`recvfrom`、`connect`、`accept`、`poll`、`gethostbyname`

handler 内直接调用即可，hook 自动生效。

---

## 性能对比

### main（3 线程，无协程）vs feat-libco（16 线程，有协程）

| 并发 | 路由 | main (RPS) | feat-libco (RPS) | 对比 |
|------|------|-----------|-----------------|------|
| 1 | GET / | 15,107 | 10,401 | main +45% |
| 10 | GET / | 49,674 | 27,087 | main +83% |
| 50 | GET / | 61,057 | 46,594 | main +31% |
| 100 | GET / | 63,098 | 57,917 | main +9% |
| 200 | GET / | 61,869 | **63,942** | libco +3% |

详细数据见 [doc/performance-comparison.md](doc/performance-comparison.md)。

### 适用场景

| 场景 | 无 libco | 有 libco |
|------|---------|---------|
| 纯计算／内存操作 | handler 立即返回 | 协程创建+切换开销，略慢 |
| handler 内读文件 | 阻塞整个线程 | yield 等待，不阻塞 |
| handler 内 DNS 查询 | 阻塞整个线程 | yield 等待，不阻塞 |
| 上游 HTTP 调用 | 阻塞整个线程 | yield 等待，不阻塞 |

---

## 构建依赖

- Linux (epoll)
- CMake >= 2.8
- C++11 编译器
- pthread, dl

---

## 项目文件结构

```
├── src/              # 网络核心 + CoHttpServer + HTTP 层
├── include/          # 头文件
├── log/              # 异步日志
├── memory/           # 内存池
├── libco/            # 协程库（libco，Tencent开源）
├── benchmarks/       # 性能测试
├── tests/            # 单元测试
└── doc/              # 文档
```

---

## 更多文档

- [libco 集成指南](doc/libco-integration-guide.md) — 完整集成过程、踩坑记录
- [异步日志系统详解](doc/async-logging-tutorial.md)
- [性能对比分析](doc/performance-comparison.md) — 含完整基准数据

---

## 知识星球

本项目在[知识星球](https://programmercarl.com/other/kstar.html)中以文字专栏形式提供完整讲解，包括：

- 项目框架梳理与架构设计
- 各模块核心代码讲解
- 面试常见问题与回答思路
- 简历写法指导
- 专属答疑群

![](https://file1.kamacoder.com/i/web/2025-09-26_11-30-13.jpg)
