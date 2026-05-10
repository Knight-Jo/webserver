# 简历写法指南

## 项目概述

基于 Linux 平台的轻量级 HTTP 服务器，采用 Reactor 模型，集成 libco 协程库及 epoll 提供高效的 I/O 处理能力。

- **基础版（main 分支）**：经典 one-loop-per-thread Reactor，16 个工作线程，支持 HTTP/1.0 路由分发
- **协程版（feat/libco-integration 分支）**：集成 libco 协程库，handler 内阻塞 I/O 自动 yield，不阻塞线程

---

## 简历模板

### 写法一：基础版

```
项目：kama-webserver — 高性能 HTTP 服务器         2025.xx - 2025.xx

技术栈：C++11 · epoll · Reactor · 多线程 · HTTP/1.0 · CMake

主要职责：
- 高并发模型：基于 One Loop per Thread 思想实现主从 Reactor 架构。
  主 Reactor 负责监听连接并轮询分发，从 Reactor 接管 I/O 事件处理，
  底层基于 epoll 多路复用，使用 eventfd 完成跨线程唤醒
- 异步日志：设计异步日志模块，采用双缓冲机制，分离前端业务落盘与
  后端 I/O 线程，吞吐量提高 2.5 倍
- HTTP 解析：实现完整 HTTP 请求解析状态机（请求行 → 头部 → body），
  支持 Keep-Alive 长连接和 query string 解析
- 内存池：实现固定大小 slot 分配器（8-512 字节，8 字节对齐），
  减少小对象频繁 malloc/free 导致的内存碎片
- LFU 缓存：实现带频率衰减的 LFU 淘汰策略，防止热点数据固化
- 使用 Apache Bench 压测：16 线程下 64,000+ RPS（GET /，并发 200），
  延迟 3.1ms
```

### 写法二：协程版

```
项目：kama-webserver — 协程化高性能 HTTP 服务器     2025.xx - 2025.xx

技术栈：C++11 · libco · epoll · Reactor · 协程 · HTTP/1.0

主要职责：
- 高并发模型：基于 One Loop per Thread 实现主从 Reactor 架构，
  在从 Reactor 线程中引入 libco 协程，底层基于 epoll 多路复用。
  主 Reactor 负责监听连接并轮询分发，从 Reactor 接管 I/O 事件，
  每个 handler 内的阻塞 I/O 被 libco hook 自动 yield，不阻塞线程
- 协程调度：设计 CoHttpServer 层，每个 sub-Reactor 持有独立 libco
  环境，EventLoop 迭代末尾调用 co_schedule_tick() 非阻塞驱动协程调度
- 协程安全发送：实现 sendCo() 接口，内核发送缓冲区满时通过 co_poll
  yield 等待 POLLOUT，避免忙等
- 异步日志：设计异步日志模块，采用双缓冲机制，分离前端业务落盘与
  后端 I/O 线程，吞吐量提高 2.5 倍
- 阻塞 I/O 验证：新增演示路由（/resolve DNS 查询、/fetch 反向代理、
  /pingback 自连接），验证协程 yield 对同线程其他请求的隔离效果
- 解决集成关键问题：extern "C" 符号链接失败、TLS 重定位错误
  （R_X86_64_TPOFF32）、--whole-archive 符号导出
```

### 写法三：组合版（突出协程深度）

```
项目：kama-webserver — 基于 libco 协程的 Web 服务器    2025.xx - 2025.xx

技术栈：C++11 · libco · epoll · Reactor · 协程 · HTTP/1.0 · CMake · gperftools

项目描述：
设计并实现一个基于 Linux 平台的轻量级 HTTP 服务器，采用 Reactor 模型，
集成 libco 协程库及 epoll 提供高效的 I/O 处理能力。handler 内阻塞 I/O
被 libco hook 自动 yield，不阻塞线程。在 16 线程+100 并发下达到 60,000+ RPS。

主要职责：
- 高并发模型：基于 One Loop per Thread 实现主从 Reactor 架构。
  主 Reactor 负责监听连接并轮询分发，从 Reactor 接管 I/O 事件处理。
  在从 Reactor 线程中引入 libco 协程，通过 epoll 多路复用驱动协程调度
- 协程集成：主导 libco 协程库的集成设计与实现，包括 CMake 构建配置、
  EventLoop 调度集成、CoHttpServer 协程处理层。采用私有栈（128KB/协程），
  参数堆分配避免悬空指针，hook 在协程入口处通过 co_enable_hook_sys() 启用
- 异步日志：设计异步日志模块，采用双缓冲机制，分离前端业务落盘与
  后端 I/O 线程，吞吐量提高 2.5 倍
- 解决集成关键问题：extern "C" 符号链接失败、TLS 重定位错误
  （R_X86_64_TPOFF32）、--whole-archive 符号导出
- 验证方案：设计阻塞 I/O 路由验证 yield 隔离效果，编写 benchmark 脚本
  实现一键分支对比测试，修复 WSL 下 gethostbyname 导致的 inet_ntop 崩溃
```

### 写法四：精炼版（聚焦 1+2+3）

```
项目：kama-webserver — 基于 libco 协程的高性能 HTTP 服务器  2025.xx - 2025.xx

技术栈：C++11 · libco · epoll · Reactor · 多线程 · HTTP/1.0

主要职责：
- 高并发模型：基于主从 Reactor + epoll 实现高并发网络架构，16 线程
  并行处理，每线程集成 libco 协程并发调度，支撑 64,000+ RPS（并发 200）
- 协程透明集成：通过 dlsym hook 拦截 read/write/connect/recv 等系统
  调用，handler 内阻塞 I/O 自动 yield 不阻塞线程。在 EpollPoller 驱动下
  实现 yield 后即时调度，协程上下文切换仅 ~50-100ns
- 异步日志：设计双缓冲异步日志系统，分离前端业务无锁追加与后端 I/O
  线程批量落盘，避免高并发下磁盘 I/O 竞争，吞吐量提高 2.5 倍
```

适用于：**一页简历 + 岗位偏向 C++ 后端/基础架构**，每条都包含「方案+实现+量化」三要素。

---

## 简历编写建议

### 1. 项目名称选择

- 可用「高性能 HTTP 服务器」或「kama-webserver」作为项目名
- 如果简历上已经有 webserver，可以加限定词区分：如「协程化 Web 服务器」

### 2. 数据指标

- **基础版**：（16 线程，GET /，并发 200）64,188 RPS，延迟 3.1ms
- **协程版**：（16 线程，GET /，并发 200）63,942 RPS，延迟 3.1ms
- **异步日志**：双缓冲分离前后端，吞吐量提高 2.5 倍（异步 ~8,000,000 msg/s vs 同步 ~3,000,000 msg/s）
- **阻塞 I/O 场景**：/resolve ~6ms，/pingback ~207ms，/fetch ~510ms
- 写 RPS 时注明测试条件（线程数、并发数、请求类型），避免面试被质疑

### 3. 技能映射

| 简历要点 | 对应考察点 |
|---------|-----------|
| 主从 Reactor / one loop per thread | 网络编程模型、多线程架构 |
| epoll / eventfd / timerfd | Linux 多路复用机制 |
| libco 协程 / hook / yield | 协程原理（栈、上下文切换、调度） |
| 双缓冲异步日志（2.5x 提升） | 生产者-消费者模型、无锁设计 |
| 状态机解析 HTTP | 有限状态机设计、协议理解 |
| TLS 重定位 R_X86_64_TPOFF32 | 编译链接原理、PIC |
| gethostbyname → getaddrinfo | 线程安全、平台兼容性 |
| Apache Bench / benchmark.sh | 性能测试、数据驱动优化 |

### 4. 常见误区

- ❌ 写「精通 C++」— 改为「熟练掌握 C++11，了解内存管理和多线程编程」
- ❌ 写「高并发服务器」没有数据支撑 — 附上具体 QPS/延迟指标
- ❌ 只堆技术名词 — 每个技术点要对应解决的问题
- ❌ 协程版和基础版混在一起写 — 面试官会混乱，确定哪个版本更匹配目标岗位

### 5. 简历结构建议

建议采用「项目描述 + 主要职责（编号列表）」的结构，面试官一目了然：

```
项目：xxx
技术栈：xxx
项目描述：一句话概括设计目标
主要职责：
1. 高并发模型：...
2. 异步日志：...
3. ...
```

每个职责点建议包含「做了什么 + 怎么做的 + 量化收益」三要素。例如：
- ❌ "实现了异步日志" → 太笼统
- ✓ "设计双缓冲异步日志，分离前端业务落盘与后端 I/O 线程，吞吐量提高 2.5 倍"

### 6. 不同岗位侧重

| 目标 | 侧重点 |
|------|--------|
| C++ 后端 | 强调网络库封装、内存池、缓存设计、RPS 数据 |
| 基础架构 / 中间件 | 强调协程集成、系统调用 hook 原理、TLS 问题排查 |
| 游戏服务器 | 强调 I/O 模型、多线程调度、定时器、异步日志 |
| 客户端（反卷） | 强调整个项目从设计到实现的完整度，展示工程能力 |
