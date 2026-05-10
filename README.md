# kama-webserver

基于 Reactor 模式的高性能 C++ Web 服务器，支持 HTTP/1.0、异步日志、内存池、LFU 缓存。

**本项目在[知识星球](https://programmercarl.com/other/kstar.html)里维护，并答疑**

---

## 架构

经典「one loop per thread」Reactor 模型：

```
mainLoop (accept 连接)
    │
    ├── subLoop 1 [epoll_wait → 处理多个连接的读写]
    ├── subLoop 2 [epoll_wait → 处理多个连接的读写]
    └── subLoop N [epoll_wait → 处理多个连接的读写]
```

### 核心模块

| 模块 | 说明 |
|------|------|
| **EventLoop** | 核心事件循环，封装 epoll_wait，通过 eventfd 实现跨线程唤醒 |
| **EPollPoller** | epoll I/O 多路复用，管理 fd 的注册/更新/删除 |
| **TcpServer** | 用户态服务端 API，管理线程池和连接生命周期 |
| **HttpServer** | HTTP 协议层，路由分发请求到业务 handler |
| **AsyncLogging** | 双缓冲异步日志，后端线程批量写入磁盘 |
| **memoryPool** | 固定大小 slot 分配器（8-512 字节），减少内存碎片 |
| **KLfuCache** | LFU 淘汰策略缓存，支持频率衰减防老化 |
| **TimerQueue** | 基于 timerfd 的定时器管理，支持一次性/重复定时器 |

---

## 快速开始

### 构建

```bash
cd build && cmake .. && make -j$(nproc)
```

### 运行

```bash
./bin/main
# 服务监听 0.0.0.0:8080
```

### 测试

```bash
# HTTP 功能测试
./build/http_tests

# 日志性能测试
./build/log_benchmark
./build/log_stress
./build/log_sync_vs_async

# HTTP 压力测试
./build/http_benchmark

# 一键性能基准测试（含分支对比）
./benchmark.sh [branch_name] [server_binary]
./benchmark.sh compare   # 自动对比 main 和 feat/libco-integration
```

---

## API 路由

### GET `/`

返回 "Hello, world"。

```bash
curl http://localhost:8080/
```

### POST `/echo`

回显请求 body。

```bash
curl -X POST -d "hello" http://localhost:8080/echo
```

### GET `/file`（阻塞 I/O 对比）

读取 `/proc/self/status`。handler 内 `read()` 是阻塞调用，会阻塞整个 EventLoop 线程。

### GET `/resolve?host=<domain>`（阻塞 I/O 对比）

DNS 解析，返回 IP 地址列表。`getaddrinfo()` 阻塞直到 DNS 查询完成。

### GET `/fetch?host=<host>&port=<port>&path=<path>`（阻塞 I/O 对比）

模拟反向代理：DNS 解析 → 创建 socket → connect → 发送 HTTP GET → recv 响应。全程阻塞。

### GET `/pingback?host=<host>&port=<port>`（阻塞 I/O 对比）

连接回自身，发送 GET / 请求并读取响应。同样全程阻塞。

> 以上阻塞 I/O 路由用于与 `feat/libco-integration` 分支的协程版本做性能对比。

---

## 性能测试

### 测试环境

- CPU: 16 核 Intel
- 内核: Linux 6.6.114.1 WSL2
- 压测工具: Apache Bench (ab)
- 测试模式: HTTP Keep-Alive
- 工作线程数: 16

### 吞吐量（GET /）

| 并发 | 请求数 | RPS | 平均延迟 |
|------|--------|-----|---------|
| 1 | 10,000 | 11,483 | 0.087ms |
| 10 | 50,000 | 27,671 | 0.361ms |
| 50 | 100,000 | 49,893 | 1.002ms |
| 100 | 200,000 | 60,353 | 1.657ms |
| 200 | 200,000 | 64,188 | 3.116ms |

详细测试数据见 `bench_results/` 目录。

---

## 构建依赖

- Linux (epoll)
- CMake >= 2.8
- C++11 编译器
- pthread

可选：gperftools（CPU profiling，cmake 自动检测）

---

## 项目文件结构

```
├── src/              # 网络核心 + HTTP 层源码
├── include/          # 头文件
├── log/              # 异步日志模块
├── memory/           # 内存池模块
├── benchmarks/       # 性能测试
├── tests/            # 单元测试
├── libco/            # 协程库（feat/libco-integration 分支）
├── doc/              # 文档
└── build/            # 构建目录
```

---

## 更多文档

- [异步日志系统详解](doc/async-logging-tutorial.md)
- [libco 集成指南](doc/libco-integration-guide.md)（feat/libco-integration 分支）
- [性能对比分析](doc/performance-comparison.md)

---

## 知识星球

本项目在[知识星球](https://programmercarl.com/other/kstar.html)中以文字专栏形式提供完整讲解，包括：

- 项目框架梳理与架构设计
- 各模块核心代码讲解
- 面试常见问题与回答思路
- 简历写法指导
- 专属答疑群

![](https://file1.kamacoder.com/i/web/2025-09-26_11-30-13.jpg)
