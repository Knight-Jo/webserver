# 简历写法指南

## 项目概述

kama-webserver 是一个基于 Reactor 模型的高性能 C++ Web 服务器。包含两个版本：

- **基础版（main 分支）**：经典 one-loop-per-thread Reactor，16 个工作线程，支持 HTTP/1.0 路由分发
- **协程版（feat/libco-integration 分支）**：集成 libco 协程库，handler 内阻塞 I/O 自动 yield，不阻塞线程

---

## 简历模板

### 写法一：基础版

```
项目：kama-webserver — 高性能 HTTP 服务器         2025.xx - 2025.xx

技术栈：C++11 · epoll · Reactor · 多线程 · HTTP/1.0 · CMake

- 基于「one loop per thread」Reactor 模型实现事件驱动架构，主 Reactor 
  accept 连接，通过轮询分发到多个 sub-Reactor 线程处理 I/O，支撑高并发
- 封装 epoll 多路复用实现非阻塞 I/O，使用 eventfd 完成跨线程唤醒，
  支持定时器事件和自定义事件分发
- 实现完整 HTTP 请求解析状态机（请求行 → 头部 → body），支持
  Keep-Alive 长连接和 query string 解析
- 设计双缓冲异步日志系统，前端无锁写入，后端线程批量 flush 磁盘，
  避免高并发下日志 I/O 竞争
- 实现内存池（8-512 字节固定 slot 分配器）减少内存碎片，
  LFU 缓存支持频率衰减防老化
- 使用 Apache Bench 压测：16 线程下 64,000+ RPS（GET /，并发 200），
  延迟 3.1ms
```

### 写法二：协程版

```
项目：kama-webserver — 协程化高性能 HTTP 服务器     2025.xx - 2025.xx

技术栈：C++11 · libco · epoll · Reactor · 协程 · HTTP/1.0

- 在 Reactor 模型基础上集成腾讯 libco 协程库，为每个 HTTP 请求创建
  独立协程，handler 内部阻塞 I/O 调用（read/write/connect/recv）被
  libco hook 透明拦截 → 自动 yield 协程 → 就绪时恢复，不阻塞线程
- 设计 CoHttpServer 层，每个 sub-Reactor 持有独立 libco 环境，
  EventLoop 迭代末尾调用 co_schedule_tick() 非阻塞驱动协程调度
- 实现协程安全发送接口 sendCo()，内核发送缓冲区满时通过 co_poll 
  yield 等待 POLLOUT，避免忙等
- 新增阻塞 I/O 演示路由（/resolve DNS 查询、/fetch 反向代理、
  /pingback 自连接），验证协程 yield 对同线程其他请求的隔离效果
- libco 协程采用私有栈（128KB/协程），参数使用堆分配避免栈帧切换
  导致悬空指针，系统调用 hook 在协程入口通过 co_enable_hook_sys() 启用
- 解决集成过程中的关键问题：extern "C" 符号链接失败、TLS 重定位错误
  （R_X86_64_TPOFF32）、--whole-archive 符号导出
```

### 写法三：组合版（突出协程深度）

```
项目：kama-webserver — 基于 libco 协程的 Web 服务器    2025.xx - 2025.xx

技术栈：C++11 · libco · epoll · Reactor · 协程 · HTTP/1.0 · CMake · gperftools

项目描述：
基于 Reactor 模型的高性能 C++ Web 服务器，集成腾讯 libco 协程库。
当 handler 中执行阻塞 I/O（read、write、connect、recv 等）时，
libco 通过 hook 机制拦截系统调用 → 自动 yield 当前协程 → 线程继续
处理其他就绪事件 → I/O 就绪时恢复协程。在 16 线程+100 并发下达到
60,000+ RPS，阻塞 I/O 路由下同线程其他请求不受影响。

个人贡献：
- 主导 libco 协程库的集成设计与实现，包括 CMake 构建配置、EventLoop
  调度集成、CoHttpServer 协程处理层
- 解决 3 个关键集成问题（extern "C" 符号、TLS 重定位、whole-archive
  导出），编写集成指南文档
- 设计阻塞 I/O 路由验证方案，编写 benchmark 脚本实现一键分支对比测试
- 将 gethostbyname 替换为 getaddrinfo，修复 WSL 平台下 DNS 解析导致
  inet_ntop 崩溃的问题
```

---

## 简历编写建议

### 1. 项目名称选择

- 可用「高性能 HTTP 服务器」或「kama-webserver」作为项目名
- 如果简历上已经有 webserver，可以加限定词区分：如「协程化 Web 服务器」

### 2. 数据指标

- **基础版**：（16 线程，GET /，并发 200）64,188 RPS，延迟 3.1ms
- **协程版**：（16 线程，GET /，并发 200）63,942 RPS，延迟 3.1ms
- **阻塞 I/O 场景**：/resolve ~6ms，/pingback ~207ms，/fetch ~510ms
- 写 RPS 时注明测试条件（线程数、并发数、请求类型），避免面试被质疑

### 3. 技能映射

| 简历要点 | 对应考察点 |
|---------|-----------|
| Reactor / one loop per thread | 网络编程模型理解 |
| epoll / eventfd / timerfd | Linux 多路复用机制 |
| 状态机解析 HTTP | 有限状态机设计能力 |
| libco hook / co_create / co_resume | 协程原理（栈、上下文切换） |
| 双缓冲异步日志 | 生产者-消费者模型 |
| TLS 重定位 R_X86_64_TPOFF32 | 编译链接原理 |
| Apache Bench / wrk | 性能测试工具使用 |

### 4. 常见误区

- ❌ 写「精通 C++」— 改为「熟练掌握 C++11，了解内存管理和多线程编程」
- ❌ 写「高并发服务器」没有数据支撑 — 附上具体 QPS/延迟指标
- ❌ 只堆技术名词 — 每个技术点要对应解决的问题
- ❌ 协程版和基础版混在一起写 — 面试官会混乱，确定哪个版本更匹配目标岗位

### 5. 不同岗位侧重

| 目标 | 侧重点 |
|------|--------|
| C++ 后端 | 强调网络库封装、内存池、缓存设计、RPS 数据 |
| 基础架构 / 中间件 | 强调协程集成、系统调用 hook 原理、TLS 问题排查 |
| 游戏服务器 | 强调 I/O 模型、多线程调度、定时器、异步日志 |
| 客户端（反卷） | 强调整个项目从设计到实现的完整度，展示工程能力 |
