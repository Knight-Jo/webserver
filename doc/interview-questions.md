# 面试问题与回答

本文档覆盖 kama-webserver 项目中可能被问到的高频面试题，包含基础版（main 分支）和协程版（feat/libco-integration 分支）两套体系的考察点。

---

## 一、网络模型与架构

### Q1：解释一下你的服务器使用了什么网络模型？

**关键词**：Reactor、one loop per thread、epoll

**回答**：

我的服务器使用经典的「one loop per thread」Reactor 模型。具体来说：

1. **main Reactor**（主 EventLoop）：负责 accept 新连接。每次 accept 后通过轮询（Round-Robin）分发给一个 sub-Reactor 线程。
2. **sub Reactor**（多个 EventLoop 线程）：每个线程运行独立的 EventLoop，使用 epoll 管理该线程上所有连接的 I/O 事件（读/写/错误）。
3. 每个 EventLoop 内部是「wait-and-dispatch」循环：
   ```
   epoll_wait → 遍历就绪 Channel → 回调 handleEvent → doPendingFunctors
   ```

这种模型的优势是：
- 连接隔离：一个连接上的耗时操作不会影响其他线程上的连接（但不隔离同线程）
- 可伸缩：增加工作线程数可以提升并发处理能力
- 无锁设计为主：大部分数据是线程局部的，跨线程通信通过 eventfd + pending functor 完成

### Q2：为什么选择 epoll 而不是 select/poll？

**关键词**：O(1) 就绪通知、mmap 加速、100 万并发

**回答**：

- **select**：fd_set 有 1024 上限，每次调用需要从用户态拷贝全部 fd 到内核，O(n) 遍历
- **poll**：解决了 1024 上限，但仍然是 O(n) 遍历全部 fd
- **epoll**：注册 fd 时只拷贝一次（epoll_ctl），内核用回调机制维护就绪链表，调用 epoll_wait 时只返回就绪的 fd，复杂度 O(1)。底层通过 mmap 加速内核和用户态的数据共享

对于 Web 服务器场景（大量空闲连接 + 少量活跃连接），epoll 的优势非常明显。

### Q3：你如何处理多线程之间的资源竞争？EventLoop 之间如何通信？

**关键词**：eventfd、pending functor、无锁队列、wakeup

**回答**：

EventLoop 之间不共享数据，需要跨线程操作时通过以下方式：

1. **eventfd**：每个 EventLoop 持有一个 eventfd，其他线程可以向该 EventLoop 的 eventfd 写入一个字节来唤醒它。
2. **pending functor**：跨线程调用 `runInLoop` 或 `queueInLoop` 时，将回调函数包装为 pending functor 放入目标 EventLoop 的队列，然后通过 eventfd 唤醒目标线程执行。
3. 这种设计保证了每个 EventLoop 线程内的数据是单线程访问，无需加锁。只有 pending functor 队列本身需要 mutex 保护（因为可能被多线程同时写入）。

### Q4：TcpConnection 的生命周期是如何管理的？

**关键词**：shared_ptr、enable_shared_from_this、Channel 回调解注册

**回答**：

TcpConnection 使用 `std::shared_ptr` 管理生命周期，继承 `enable_shared_from_this` 以便在回调中安全地获取自身引用。

关键点：
1. 连接建立时，TcpServer 创建 TcpConnection 对象并存储在线程池的 ConnectionMap 中。
2. Channel 的各个回调（handleRead、handleWrite、handleClose 等）绑定到 TcpConnection 的成员函数，回调中持有 shared_ptr 保证对象在使用期间不被销毁。
3. 连接关闭时（对端关闭或发生错误），handleClose 触发移除 Channel 的 epoll 注册，并从 ConnectionMap 中删除，shared_ptr 引用归零后自动析构。
4. 协程版中需要额外注意：协程内部持有 TcpConnection 引用时，连接可能在协程 yield 期间关闭。sendCo 中通过 `state_ == kConnected` 检查来规避。

---

## 二、HTTP 协议

### Q5：HTTP 解析是怎么实现的？如何解析 HTTP body？

**关键词**：有限状态机、Content-Length、Transfer-Encoding、Reader index

**回答**：

HTTP 解析是一个有限状态机，逐字节推进：

```
kExpectRequestLine → kExpectHeaders → kExpectBody → kGotAll
```

1. **请求行**：解析 Method、URI、Query String、Version。正常解析到 `\r\n` 为止。
2. **头部**：逐行解析 key:value，存入 `std::map`。遇到空行（`\r\n\r\n`）说明头部结束。
3. **body**：根据头部确定 body 长度：
   - 优先 `Content-Length`：读取指定字节数
   - `Transfer-Encoding: chunked`：按 chunk 读取（当前简化版未实现）
   - GET/HEAD 请求无 body

状态机使用 Buffer 的 reader index 和 writer index 管理已解析和未解析的数据，避免数据拷贝。

### Q6：Keep-Alive 如何实现的？Connection: close 如何处理？

**关键词**：HTTP/1.0 默认短连接、Connection 头字段、状态判断

**回答**：

- HTTP/1.0 默认是短连接（Connection: close），每次请求后关闭连接
- 如果请求头包含 `Connection: Keep-Alive`，响应中也设置 `Connection: Keep-Alive`，连接不关闭继续处理下一个请求
- 服务端通过 `HttpResponse::closeConnection()` 判断是否关闭连接

---

## 三、协程与 libco（协程版特有）

### Q7：libco 是怎么 hook 系统调用的？hook 的原理是什么？

**关键词**：dlsym、RTLD_NEXT、syscall 转发、__poll

**回答**：

libco 通过 `dlsym(RTLD_NEXT, "read")` 获取 libc 中真实的 `read` 函数指针，然后用自己的 hook 函数替换。

hook 后的 `read()` 大致流程：

```c
// hook 后的 read()
ssize_t read(int fd, void *buf, size_t count) {
    // 检查是否在协程环境中且 hook 已启用
    if (!co_is_enable_sys_hook()) {
        return g_sys_read_func(fd, buf, count);  // 直接调用原始 read
    }
    // 将 fd 设为非阻塞
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    ssize_t ret = g_sys_read_func(fd, buf, count);
    if (ret >= 0 || errno != EAGAIN)
        return ret;
    
    // fd 不可读，注册 epoll + yield
    pollfd pf = {.fd = fd, .events = POLLIN};
    co_poll_inner(epoll_ct, &pf, 1, timeout, NULL);
    // 恢复后重试
    return g_sys_read_func(fd, buf, count);
}
```

核心技巧：
- `RTLD_NEXT` 是 GNU 扩展，用于 dlsym 查找下一个（原始）符号
- 先将 fd 设为非阻塞，然后尝试调用——如果返回 EAGAIN 再 yield
- yield 后线程回到 EventLoop，协程上下文保存在私有栈中

### Q8：协程的上下文切换是怎样的？co_create 和 co_resume 做了什么？

**关键词**：ucontext、setjmp/longjmp、coctx、寄存器保存恢复

**回答**：

libco 使用汇编实现上下文切换，比 ucontext 更轻量：

- **co_create**：分配协程栈（私有栈默认 128KB），设置初始上下文（指令指针、栈指针），初始状态为就绪
- **co_resume**：保存当前协程的寄存器（rsp、rbp、rip 等）到当前上下文结构体，然后恢复目标协程的寄存器，跳转到其执行位置。核心在 `coctx_swap.S` 中用汇编实现：
  ```asm
  // x86-64 简化示意
  movq %rsp, 16(%rdi)   // 保存当前 rsp 到 from->regs[2]
  movq 16(%rsi), %rsp   // 恢复目标 rsp
  pushq 8(%rsi)          // 将目标 rip 压栈
  ret                    // 弹出 rip 跳转执行
  ```

整个切换过程只涉及寄存器保存/恢复，不涉及系统调用，是纯用户态操作，耗时约 50-100ns。

### Q9：协程栈为什么选 128KB？能用共享栈减少内存吗？

**关键词**：私有栈 vs 共享栈、内存开销、栈溢出

**回答**：

- **私有栈 128KB**：每个协程独立分配 128KB 栈空间。1000 个并发协程约 128MB，对于 16 核服务器可以接受。优点是实现简单、线程安全，缺点是栈空间浪费（实际多数协程只使用几 KB）。
- **共享栈**：多个协程共享同一个栈空间，yield 时将栈内容拷贝到堆上保存，resume 时拷贝回来。优点是大幅节省内存，缺点是有拷贝开销、需要处理栈回收的复杂度。
- 如果服务器的并发连接数很高（万级别），可以改为共享栈模式。当前版本优先选择简单可靠的私有栈。

### Q10：协程中参数传递有什么注意事项？为什么需要堆分配？

**关键词**：栈帧切换、悬空指针、yield 后栈被覆盖

**回答**：

协程参数必须堆分配，不能传递栈上局部变量的指针。原因是：

当协程 A 调用 `co_yield()` 后，线程可能 resume 另一个协程 B。此时线程的栈变为 B 的栈（B 的私有栈或共享栈），A 原本的栈帧内容被覆盖。

```cpp
// 错误 ❌ — 局部变量 arg 在栈上
void onMessage() {
    CoHandlerArg arg;      // 在 onMessage 的栈帧中
    co_create(&co, ..., handlerRoutine, &arg);
    co_resume(co);          // handlerRoutine 内 yield
    // onMessage 返回后 arg 就没了
}

// 正确 ✓ — 堆分配
void onMessage() {
    CoHandlerArg *arg = new CoHandlerArg();
    co_create(&co, ..., handlerRoutine, arg);
    co_resume(co);
    // handlerRoutine 里 delete arg
}
```

### Q11：co_schedule_tick 为什么必须是非阻塞的？为什么 EventLoop 的 kPollTimeMs 要改为 100ms？

**关键词**：协作式调度、协程唤醒延迟

**回答**：

**co_schedule_tick 必须非阻塞**：因为它是在 EventLoop 的 loop 循环中调用的，如果阻塞就会阻塞整个 EventLoop 的事件处理。所以调用 `co_epoll_wait(epfd, result, size, 0)` 时超时参数为 0（立即返回），只处理已经就绪的事件和超时事件。

**kPollTimeMs = 100ms**（原值 10000ms）：原值 10 秒意味着 EventLoop 的主 epoll_wait 最长阻塞 10 秒。如果协程在 `recv()` 中 yield 等待 POLLIN，即使 I/O 数据在微秒级内到达，EventLoop 也要等最长 10 秒才会被唤醒 → 调用 co_schedule_tick → co_epoll_wait 发现就绪 → co_resume 恢复协程。改为 100ms 后，协程恢复延迟降至 ~100ms。正确做法应该是将 libco 的 epoll fd 注册到 EventLoop 的 epoll 中，这样 I/O 到达时能立即唤醒。

### Q12：线程和协程有什么区别？什么场景用线程，什么场景用协程？

**关键词**：内核态 vs 用户态、抢占式 vs 协作式、调度开销

**回答**：

| 对比维度 | 线程 | 协程 |
|---------|------|------|
| 调度者 | 内核（操作系统） | 用户态程序自己控制 |
| 调度方式 | 抢占式（时间片轮转） | 协作式（主动 yield） |
| 切换开销 | ~1μs（系统调用 + 上下文切换） | ~50-100ns（仅寄存器保存恢复） |
| 栈大小 | 默认 8MB | 自定义（libco 128KB） |
| 创建/销毁开销 | 高（涉及内核） | 低（纯用户态） |
| 并行能力 | 真并行（多核同时执行） | 单线程内并发（非并行） |

**使用场景**：

- **线程**：CPU 密集型计算（多核并行）、需要真并行的场景、阻塞系统调用（传统方式）
- **协程**：I/O 密集型任务（网络请求、文件读写）、大量并发连接、需要维护大量状态机

**为什么这个项目选择线程+协程结合**：16 个工作线程提供并行能力，每个线程内的协程提供并发能力。线程负责「并行处理」，协程负责「等待 I/O 时不阻塞」。

### Q13：subReactor 为什么使用「线程 + 协程」的组合，而不是直接用协程替代线程作为 subReactor？

**关键词**：并行 vs 并发、多核利用、CPU 密集、协程不抢占

**回答**：

这是一个非常好的架构问题。核心答案是：**协程只能提供并发（concurrency），不能提供并行（parallelism）。而 subReactor 需要并行。**

具体分析：

**方案一：纯协程（单线程 + 多协程）**

```
main Reactor（accept）
    │
    └── 1 个线程 = 1 个 EventLoop
         ├── 协程 A（处理连接 1）
         ├── 协程 B（处理连接 2）
         └── 协程 C（处理连接 3）
```

问题：
- **只能利用 1 个 CPU 核**：所有连接的处理都挤在一个线程上。16 核的机器浪费 15 个核
- **CPU 密集任务阻塞所有协程**：如果某个协程做耗时计算（JSON 序列化、加解密、数据聚合），它不 yield，其他所有协程都在等它
- **协程无法抢占**：协作式调度意味着一个不 yield 的协程会饿死同线程上的所有其他连接

**方案二：纯线程（多线程 + 无协程）**

```
main Reactor（accept）
    │
    ├── subReactor 线程 1（epoll）
    ├── subReactor 线程 2（epoll）
    └── subReactor 线程 N（epoll）
```

问题：
- 每个 handler 中的阻塞 I/O 都会阻塞整个线程
- 需要大量线程来应对阻塞场景，线程数 = 并发连接数，不可行

**方案三：线程 + 协程（本项目的选择）**

```
main Reactor（accept）
    │
    ├── subReactor 线程 1（epoll + 协程调度）
    │    ├── 协程 A（连接 1 的 handler）
    │    └── 协程 B（连接 2 的 handler）
    ├── subReactor 线程 2（epoll + 协程调度）
    └── subReactor 线程 N（epoll + 协程调度）
```

优势：
1. **多核利用**：16 个线程分布在多个核上，实现真正的并行处理
2. **阻塞不阻塞线程**：协程 yield 让出 CPU，该线程上的其他协程继续执行
3. **线程数可控**：线程数 = CPU 核数（或略多），不随连接数增长
4. **连接数可扩展**：每个线程可挂载数千个协程，总并发 = 线程数 × 每线程协程数

**画龙点睛的比喻**：

> 把 CPU 核想象成餐厅的厨师（线程），把每个请求想象成一道菜（协程）。
> - 方案一（纯协程）：1 个厨师做所有菜，遇到要等烤箱的就停下手里的活，但烤箱工作时他干等着
> - 方案二（纯线程）：每道菜配一个厨师，厨师多的时候厨房挤不下，厨师少的时候菜做不完
> - 方案三（线程+协程）：16 个厨师，每人管一排灶台。遇到要等烤箱的菜，把菜放烤箱上，先做下一道。烤箱好了再回来继续。**这正是本项目的方式**

**极端情况思考**：如果服务器只有 1 个 CPU 核，方案一就足够了。但现实服务器的 CPU 核数从 4 到 128 不等，必须用线程来利用多核能力。Go 语言的 goroutine 本质上也是「线程（M）+ 协程（G）」模型，其调度器（G-M-P）与我们的设计异曲同工——只不过 Go 的调度是运行时自动完成的。

### Q14：libco 内部有两个 epoll 实例——libco 的 epoll 和 EventLoop 的 epoll，为什么需要两个？能不能合并？

**关键词**：关注点分离、唤醒路径、epoll fd 合并

**回答**：

两个 epoll 实例各自负责不同的事件：

| epoll 实例 | 所属 | 负责事件 |
|-----------|------|---------|
| EventLoop 的 EPollPoller | 应用程序 | accept、channel I/O（读/写）、timerfd、eventfd |
| libco 的 epoll（co_get_epoll_ct） | libco 库 | 协程内被 hook 的 fd（等待 POLLIN/POLLOUT 的 socket） |

**为什么需要两个**：
- libco 的 epoll 由 hook 函数内部注册（`co_poll_inner` → `epoll_ctl(libco_epfd)`），应用层代码不需要手动管理
- 如果合并，需要修改 libco 源码使其使用 EventLoop 的 epoll，侵入性大

**能否合并**：可以，而且是推荐的优化方向。将 libco 的 epoll fd 注册到 EventLoop 的 epoll 中（监听读事件），当协程等待的 fd 就绪时立即唤醒 EventLoop。好处：
- 消除 co_schedule_tick 的轮询延迟（目前每 100ms 才检查一次）
- I/O 到达时微秒级唤醒协程
- 减少 CPU 空转（目前每轮 EventLoop 都调用一次 co_epoll_wait，即使没有协程活跃）

### Q15：协程从创建到销毁经历了哪些生命周期阶段？

**关键词**：co_create、co_resume、co_yield、协程状态机

**回答**：

```
                  co_create()
                      │
                   ┌──▼──┐
          ┌────────│ 就绪 │
          │        └──┬──┘
          │           │ co_resume()
          │        ┌──▼──┐       阻塞 I/O 到达
          │        │ 运行 ├────────────────────┐
          │        └──┬──┘                    │
          │           │ handler 返回          │
          │        ┌──▼──┐                   │
          │        │ 结束 │                   │
          │        └─────┘                   │
          │         co_release()              │
          │                                   │
          │            co_poll() / hook 触发 yield
          │                        │
          │                    ┌───▼────┐
          │                    │ 挂起/等待 │◄─────────── co_schedule_tick()
          │                    │  (阻塞)  │     epoll 就绪或超时后 co_resume()
          │                    └─────────┘
          │
          └───── 被 co_eventloop 主动 yield（同线程协程过多时）
```

具体阶段：
1. **co_create**：分配协程栈和 stCoRoutine_t 结构体，设置入口函数和初始上下文。此时协程在「就绪」状态。
2. **co_resume（首次）**：切换到协程上下文，执行入口函数（coHandlerRoutine）。
3. **co_enable_hook_sys**：开启系统调用 hook（必须在协程内调用）。
4. **运行 handler**：执行业务逻辑。
5. **co_yield（hook 触发）**：当 handler 内调用 `read()` 等系统调用且 fd 不可用时，libco hook 自动调用 `co_poll_inner` → `co_yield`。协程进入「挂起」状态，线程回到 EventLoop。
6. **co_resume（恢复）**：`co_schedule_tick()` 或 `co_eventloop_tick()` 发现 fd 就绪，调用 `co_resume` 恢复协程执行。
7. **handler 返回**：路由执行完成，`sendCo()` 发送响应。
8. **协程退出**：入口函数返回，libco 内部调用 `co_release()` 释放协程栈和上下文。

### Q16：libco hook 了哪些系统调用？哪些常见调用没有被 hook？如果需要 hook 一个额外的系统调用怎么做？

**关键词**：hook 列表、未覆盖调用、dlsym + 封装

**回答**：

**已被 libco hook 的系统调用**：
- I/O：`read`、`write`、`readv`、`writev`、`send`、`recv`、`sendto`、`recvfrom`、`sendmsg`、`recvmsg`
- 连接：`connect`、`accept`、`accept4`
- 多路复用：`poll`、`select`
- 其他：`close`、`gethostbyname`、`gethostbyname2`、`gethostbyaddr`

**未被 hook 但值得关注的调用**：
- `sleep`、`usleep`、`nanosleep`：如果 handler 中调用，会阻塞整个线程（不会 yield）
- `fsync`、`fdatasync`：同步文件数据到磁盘，如果调用则阻塞线程
- `open`、`openat`：打开文件通常很快，但网络文件系统（NFS/CIFS）可能阻塞
- `system`、`popen`：执行外部命令，必然阻塞

**如何添加新的 hook**：
1. 在 hook 函数中使用 `dlsym(RTLD_NEXT, "nanosleep")` 获取原始函数指针
2. 编写同名 hook 函数，在协程环境中用 `co_poll` + `co_yield` 实现异步等待
3. 编译时链接到共享库，确保 LD_PRELOAD 或链接顺序优先

示例——hook `nanosleep` 的思路：
```c
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!co_is_enable_sys_hook())
        return g_sys_nanosleep_func(req, rem);
    // 用 poll 模拟 sleep：poll(NULL, 0, timeout_ms)
    // poll 本身被 hook 了，所以会 yield
    co_poll(co_get_epoll_ct(), NULL, 0, req->tv_sec * 1000 + req->tv_nsec / 1000000);
    return 0;
}
```

---

## 四、内存与缓存

### Q17：内存池的设计思路是什么？为什么不用 malloc？

**关键词**：内存碎片、固定大小 slot、freelist、8 字节对齐

**回答**：

Web 服务器频繁分配小对象（字符串、缓冲区等），malloc 存在两个问题：
1. **内存碎片**：小对象频繁分配释放，堆上出现大量无法利用的小空洞
2. **性能开销**：malloc/free 涉及内核态操作、锁竞争

内存池设计：
- 将内存划分为固定大小的 slot（8 的倍数，8B~512B）
- 每个 slot 大小的页面维护一个 free list（空闲链表），分配时从链表头取，释放时插回链表头
- 大于 512B 的对象退回到 malloc

### Q18：LFU 缓存的淘汰策略怎么实现的？和 LRU 比有什么优缺点？

**关键词**：频率计数、频次链表、平均频率衰减

**回答**：

LFU（Least Frequently Used）淘汰使用频率最低的条目。

**实现**：
- 维护一个按访问频率（freq）分组的链表结构，每次访问增加频率计数并移动到对应链表
- 引入了「平均频率衰减」机制：当所有条目的平均频率超过阈值时，统一减少计数，防止旧的热键永远不被淘汰

**LFU vs LRU**：

| 对比 | LRU | LFU |
|------|-----|-----|
| 适合场景 | 最近访问过的可能再次访问 | 经常访问的热点数据 |
| 缺点 | 偶发大量访问的条目会长期占据缓存 | 旧热点可能"固化"（频率衰减解决） |
| Web 场景 | 缓存频繁变化的内容 | 缓存稳定热点数据 |

---

## 五、日志系统

### Q19：双缓冲异步日志是怎么实现的？为什么不需要锁？

**关键词**：生产者-消费者、缓冲区交换、无锁追加

**回答**：

双缓冲异步日志的核心是「前端无锁追加，后端批量写入」：

```
前端线程 (多个)                   后端线程 (单个)
    │                              │
    ├── append(msg) ──────────→    ├── 定时（~3s）或 buffer 满
    │   写入 current buffer        │   交换 current 和备用 buffer
    ├── append(msg) ──────────→    ├── 将 current 写入文件
    │   写入 current buffer        │   fwrite_unlocked 批量 flush
    │                              │
```

**前端**：每个 Logger 对象在构造时格式化日志内容到 LogStream（固定 4KB 栈上 buffer），析构时调用 `append()` 追加到 AsyncLogging 当前 buffer。因为同一时刻只有一个线程构造 Logger（RAII），所以不需要锁。

**后端**：专一线程循环执行：
1. 加锁交换 current buffer 和一个空 buffer（仅交换指针，O(1)）
2. 将交换出的 buffer 内容批量写入文件
3. 如果写入缓冲区超过滚动大小（1MB），滚动文件

### Q20：日志写入磁盘频率过高会影响服务器性能吗？如何平衡？

**关键词**：buffer 大小、flush 间隔、异步 vs 同步

**回答**：

日志系统的设计目标就是**最小化对业务线程的影响**：

- 前端写入无锁操作，仅追加到内存 buffer（微秒级）
- 后端批量写入，默认 3 秒或 4MB 触发一次，将多次小写入合并为一次大写入（毫秒级）
- 文件写入使用 `fwrite_unlocked` 而非 `fwrite`，省略内部锁检查

实测数据（log_benchmark）：
- 同步日志：~3,000,000 msg/sec（直接影响业务线程）
- 异步日志：~8,000,000 msg/sec（业务线程仅追加 buffer）

---

## 六、协程集成实战（协程版特有）

### Q21：集成 libco 过程中遇到过哪些坑？怎么解决的？

**关键词**：extern "C"、TLS 重定位、--whole-archive

**回答**：

三个关键问题：

**1. extern "C" 导致符号链接失败**
- 症状：undefined reference，但 nm 显示符号存在
- 原因：libco 的 `.h` 不包含 `extern "C"`，编译出的是 C++ mangled 符号（`_Z23co_init_curr_thread_envv`）。用 `extern "C" { #include <co_routine.h> }` 包裹后，链接器找 unmangled 符号，自然找不到
- 修复：直接 `#include <co_routine.h>`，不加 extern "C"

**2. TLS 重定位 R_X86_64_TPOFF32**
- 症状：`relocation R_X86_64_TPOFF32 against __tls_guard cannot be used when making shared object`
- 原因：libco 使用 `static __thread`（初始 exec TLS 模型）编译出 R_X86_64_TPOFF32 重定位，但 colib_static 被链接进共享库 libsrc_lib.so 时该重定位类型无效
- 修复：cmake 中设置 `POSITION_INDEPENDENT_CODE ON`，自动加 -fPIC 使编译器使用 general-dynamic TLS 模型

**3. colib_static 符号未导出到共享库**
- 症状：链接可执行文件时报 undefined reference
- 原因：链接共享库时 ld 默认丢弃未引用符号
- 修复：`target_link_libraries(src_lib -Wl,--whole-archive colib_static -Wl,--no-whole-archive)`

### Q22：协程 handler 中如果出现阻塞死循环或死锁会怎样？

**关键词**：协程不会抢占、共享栈污染、监控超时

**回答**：

libco 是协作式协程（非抢占式），如果一个协程陷入死循环且不执行任何 hook 系统调用，它永远不会 yield，该线程上的所有其他协程和连接都会被饿死。

解决方案（在计划中未实现）：
1. 在协程中定期调用 `co_yield()` 主动让出（侵入式）
2. 使用 watchdog 线程监控 EventLoop 的响应性，如果某个线程长时间未处理新连接，强制结束或重启
3. 设置 handler 执行超时，创建协程时启动定时器，超时后强制关闭连接

实际生产环境中，建议 handler 内避免长时间 CPU 计算，CPU 密集操作应使用线程池执行。

### Q23：sendCo 和普通 send 有什么区别？什么时候需要 yield？

**关键词**：POLLOUT、epoll 边沿触发、写缓冲区满

**回答**：

普通 `send()`：如果内核发送缓冲区满，write/send 直接返回 EAGAIN。在 Reactor 模式下，需要主动关注 POLLOUT 事件，等待缓冲区可写后再写入。

`sendCo()`：在协程上下文中调用，如果 `write()` 返回 EAGAIN，不返回给 EventLoop，而是：

```cpp
// sendCo 核心逻辑
while (remaining > 0) {
    struct pollfd pf;
    pf.fd = fd;
    pf.events = POLLOUT;
    co_poll(co_get_epoll_ct(), &pf, 1, 1000);  // yield 等待 POLLOUT
    ssize_t n = ::write(fd, data, remaining);
    if (n > 0) {
        data += n;
        remaining -= n;
    }
}
```

`co_poll` 内部将 fd 注册到 libco 的 epoll，然后 yield 当前协程。当 fd 可写时（epoll 返回 POLLOUT），`co_schedule_tick()` 恢复协程，`write()` 重试。

本质上，sendCo 将异步编程中的「注册 POLLOUT → 等回调 → 写入」过程，封装成同步风格的「while + poll + write」循环，代码更直观。

### Q24：协程 yield 期间 TCP 连接关闭了怎么办？如何安全处理这种竞态？

**关键词**：连接关闭、竞态条件、shared_ptr、状态检查

**回答**：

这是协程编程中典型的竞态问题：一个协程在 `recv()` 中 yield 等待数据时，对端关闭了连接（或超时关闭），连接被销毁。协程恢复后访问已释放的 TcpConnection 对象 → 野指针崩溃。

**当前项目中的处理**：
1. **状态检查**：`sendCo()` 中每次写操作前检查 `state_ == kConnected`，如果连接已关闭直接返回
2. **shared_ptr 延长生命周期**：`sendCo` 调用前通过 `shared_from_this()` 获取共享引用，确保在 yield 期间连接对象不被析构

```cpp
void TcpConnection::sendCo(const std::string &buf) {
    auto guard = shared_from_this();  // yield 期间保持连接存活
    if (state_ != kConnected) return;
    // ... co_poll / write 循环 ...
    // guard 析构，如果连接已关闭则销毁对象
}
```

**未完全解决的问题**：
1. 如果连接在 `co_poll` 期间关闭，`co_poll` 超时返回后才发现关闭，存在延迟
2. 多线程场景下，连接关闭事件可能发生在另一个线程，需要跨线程同步状态
3. 更彻底的方案：在连接关闭时主动「唤醒」所有正在等待该 fd 的协程（通过 eventfd 写入 libco 的 epoll）

### Q25：协程内如何捕获异常？handler 抛未处理异常会怎样？

**关键词**：栈 unwinding、异常安全、协程栈析构

**回答**：

**情况分析**：

```cpp
void *coHandlerRoutine(void *arg) {
    co_enable_hook_sys();
    try {
        handler(request, response);  // 用户 handler 可能抛异常
    } catch (const std::exception &e) {
        response->setStatus(500, "Internal Error");
        response->setBody(e.what());
    }
    conn->sendCo(response.toString());
    delete arg;
    return NULL;
}
```

**如果 handler 抛出未捕获异常**：
- 异常会沿着协程栈向上传播，但 libco 的 `coHandlerRoutine` 函数指针类型是 `void* (*)(void*)`，不包含异常规格说明
- libco 的 `co_resume` 内部不会 catch 该异常，异常会继续传播到 EventLoop 的事件处理循环
- 实际上，C++ 异常在协程边界上的行为**未定义**——可能不会正确 unwinding 协程栈，导致协程栈内存泄漏

**最佳实践**：
1. **在协程入口函数内包 try-catch**：捕获所有异常，转换为 HTTP 500 响应
2. **使用 `catch (...)` 兜底**：捕获非 std::exception 派生类异常
3. **确保所有 handler 不抛异常**：通过 noexcept 声明和代码审查

```cpp
void *coHandlerRoutine(void *arg) {
    co_enable_hook_sys();
    try {
        handler(request, response);
    } catch (...) {
        response->setStatus(500, "Internal Server Error");
        response->setBody("unhandled exception");
    }
    // ...
}
```

### Q26：协程程序如何调试？GDB 能看到协程的调用栈吗？

**关键词**：GDB 多线程、协程栈、breakpoint、coroutine-local

**回答**：

**协程调试的难点**：
- GDB 默认只看到线程的栈帧（即当前正在运行的协程的栈），yield 状态的协程栈不在线程栈上（在堆上分配的私有栈中）
- 普通的 `bt`（backtrace）只能看到当前线程的调用栈

**调试方法**：

1. **运行时日志（最有效）**：在 `co_create`、`co_resume`、`co_yield` 处添加日志，输出协程 ID 和事件类型
2. **GDB 下定位当前协程**：
   ```gdb
   # 查看当前正在运行的协程
   p stCoRoutineEnv_t->curr_co
   # 查看该协程的入口函数和栈地址
   p (stCoRoutineEnv_t->curr_co)->cStart
   p (stCoRoutineEnv_t->curr_co)->cCoStack
   ```
3. **查看私有栈内容**：通过协程结构体的栈基址和大小，可以 dump 堆上的协程栈
   ```gdb
   x/256gx (stCoRoutineEnv_t->curr_co)->cCoStack
   ```
4. **在关键 hook 处打断点**：在 `co_poll_inner`、`co_yield_ct` 函数处打断点，确认协程是否按预期 yield

**实用技巧**：
- 使用 `co_strerror` 或类似映射表记录协程 ID 和请求信息的对应关系，方便追踪
- 对协程栈大小做标记（填充魔数 0xDEADBEEF），观察栈使用峰值
- 在 sendCo 和 handler 入口添加 `get_co_id()` 日志，关联请求处理的全链路

### Q27：协程是线程安全的吗？多个线程可以同时操作同一个协程吗？

**关键词**：线程局部存储、协程所有权、跨线程 co_resume

**回答**：

**libco 的协程不是线程安全的**——协程始终属于创建它的线程。原因：

1. **线程局部存储**：`co_get_curr_thread_env()` 返回当前线程的 `stCoRoutineEnv_t` 结构体。`co_resume`、`co_yield` 等操作依赖于这个 TLS 值。
2. **epoll 归属**：协程的 fd 注册在所属线程的 libco epoll 实例中，其他线程不持有该 epoll fd。
3. **栈冲突**：私有栈在线程堆上分配，但不同线程同时访问同一协程的栈会导致数据竞争。

**哪些操作是危险的**：

```cpp
// 危险 ❌ — 线程 B 恢复线程 A 创建的协程
// 线程 A
co_create(&co, ...);
// 线程 B
co_resume(co);  // B 的 TLS 环境与 co 不匹配

// 线程 A 的 EpollPoller 管理 epoll fd
// 线程 B 调用 co_resume 后 hook 的 fd 会注册到 B 的 epoll，逻辑混乱
```

**正确的做法**：协程的创建、resume、yield、销毁都在同一个线程中完成。这和我们「one loop per thread」的设计一致——每个 subReactor 线程独立管理自己的协程生命周期。

**例外情况**：如果确实需要跨线程唤醒协程，可以通过 eventfd 间接实现：
1. 线程 A 的协程在等待某个条件
2. 线程 B 向线程 A 的 eventfd 写入字节
3. 线程 A 的 EventLoop 被唤醒，检查条件，恢复协程

### Q28：libco 和 C++20 的 coroutine（无栈协程）有什么区别？各有什么优劣？

**关键词**：有栈 vs 无栈、可移植性、性能和灵活性

**回答**：

| 对比 | libco（有栈协程） | C++20 coroutine（无栈协程） |
|------|-----------------|--------------------------|
| 栈 | 独立栈（128KB） | 无独立栈，使用调用者栈 |
| 切换 | 汇编保存/恢复寄存器 | 编译器生成状态机 |
| hook | 支持（dlsym 替换系统调用） | 不支持系统调用 hook（需要显式 await） |
| 语法 | 同步代码风格 | 需要 `co_await`、`co_yield` 关键字 |
| 内存 | 每个协程固定栈，开销较大 | 极小（仅状态机变量） |
| 可移植性 | Linux x86-64 汇编 | 跨平台（由编译器处理） |
| 上手难度 | 低（代码和普通函数一样） | 高（需要理解 awaiter/promise 框架） |

**libco 的优势**：
- 透明 hook：已有同步代码可以零修改迁移到协程（这是本项目选择 libco 的关键原因）
- 调试友好：GDB 可以看到完整的调用栈

**C++20 coroutine 的优势**：
- 标准化的，跨平台
- 内存开销极小
- 编译器优化更好

**为什么本项目选 libco 而非 C++20 coroutines**：
- 本项目基于 C++11，无法使用 C++20 特性
- libco 的 hook 机制透明，handler 不需要任何修改就能受益于协程——只需在 onMessage 中创建协程即可
- 系统调用 hook 是 libco 的核心能力，C++20 coroutine 不提供类似机制，需要手动将每个 I/O 操作封装为 awaitable 对象

### Q29：协程中用到了哪些 C++ RAII 惯用法需要注意的地方？

**关键词**：锁持有、栈展开、资源泄漏

**回答**：

**协程中使用 RAII 需要特别小心**，因为协程 yield 时栈帧被保留（不是 unwinding），RAII 析构函数不会在 yield 时触发：

```cpp
// 危险 ❌ — yield 期间锁不会被释放
void handler(HttpResponse *response) {
    std::lock_guard<std::mutex> lock(mutex_);  // 加锁
    // ... 
    int fd = open(...);
    read(fd, buf, ...);  // ← 被 hook → yield！
    // yield 期间锁仍然被持有！其他协程无法获取该锁
    // ...
    // 协程恢复后↓
}  // 这里才解锁，但 yield 期间锁被无效持有

// 正确 ✓ — 避免在可能 yield 的代码路径中持有锁
void handler(HttpResponse *response) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 快速操作，不包含任何可能 yield 的调用
    }
    // yield-safe 区域
    int fd = open(...);
    read(fd, buf, ...);  // yield 安全，因为没持有锁
}
```

**RAII 在协程中的注意事项**：
1. **锁**：避免在持有锁的代码路径中执行可能被 hook 的调用（read/write/connect 等）。如果确需保护跨越 yield 的临界区，使用协程感知的锁（如 `co_mutex`）
2. **文件描述符**：`open`/`close` 配对要考虑 yield 路径：如果 open 后 yield，close 前协程被销毁 → fd 泄漏。解决方案：使用 RAII 包装器，在析构函数中 close，并确保在协程入口处 catch 所有路径
3. **堆内存**：协程参数使用 `new` 分配，在协程结束时 `delete`。如果协程在参数构造后被销毁且未执行入口函数 → 内存泄漏。解决方案：使用 `std::unique_ptr` 管理堆分配参数，通过 `.release()` 转移所有权

### Q30：如果 handler 内部调用了 fork()，协程环境会发生什么？

**关键词**：fork 安全、TLS、epoll fd 继承、死锁风险

**回答**：

**fork() 在协程环境中非常危险**，问题包括：

1. **TLS 继承**：子进程继承了父进程的线程局部存储（`co_get_curr_thread_env`），但子进程只有一个线程，TLS 指向的 `stCoRoutineEnv_t` 结构体中的 epoll fd 在子进程中仍然有效（因为 epoll fd 是跨 fork 继承的），但 fd 上注册的事件可能行为异常。

2. **协程状态不一致**：如果在协程中 fork，子进程的「当前协程」状态指向父进程正在运行的协程，但该协程的栈在子进程中不可用（私有栈在堆上，继承内容但可能处于不一致状态）。

3. **锁状态**：fork 时如果其他线程持有锁，子进程无法释放这些锁（线程已消失），导致死锁。

**推荐做法**：不要在协程 handler 中调用 `fork()`。如果确需创建子进程，通过专用的线程池或进程池统一管理，避免在协程上下文中直接 fork。

**如果必须在协程中 fork**，建议在 fork 后立即 `exec()` 替换进程映像（重置所有状态），且 fork 前确保没有持有任何锁：

```cpp
void handler(HttpResponse *response) {
    // fork 前确保没有锁、没有 yield 过的 fd 处于等待状态
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程
        execlp("/bin/ls", "ls", NULL);  // 替换进程映像
        _exit(127);  // exec 失败
    } else if (pid > 0) {
        // 父进程继续
    }
}
```

---

## 七、性能与测试

### Q31：你用什么工具做压力测试？主要关注哪些指标？

**关键词**：ab、RPS、延迟、TP99、性能毛刺

**回答**：

工具：Apache Bench (ab) 和 wrk。

关注指标：
1. **RPS（Requests Per Second）**：每秒钟处理的请求数，核心吞吐量指标
2. **延迟**：平均延迟、TP99、TP999（百分位延迟）
3. **失败请求**：超时、连接重置、非 200 响应
4. **可伸缩性**：RPS 随并发增加的增长曲线，确认是否平滑扩展

测试方法：
- 按并发级别逐步测试（1、10、50、100、200）
- 先用 1000 请求预热服务器
- 记录系统信息（CPU、内存）排除资源瓶颈
- 对比分支测试：用 `benchmark.sh compare` 自动测试 main 和 feat/libco-integration 并生成对比报告

### Q32：怎么验证协程 yield 有效？阻塞 I/O handler 不会阻塞其他请求吗？

**关键词**：并发隔离测试、基线 RPS、混合 RPS

**回答**：

通过并发隔离测试验证：

```
1. 基线：只压测快请求（GET /，10 并发，1000 请求）
   记录基线 RPS
2. 混合：快请求（同上）+ 后台一个慢请求（/fetch?host=example.com）
   记录混合 RPS
3. 计算 RPS 比率 = 混合 / 基线
```

- 如果协程正常工作，RPS 比率应接近 100%（慢请求 yield，不阻塞同线程的快请求）
- 如果协程失效（阻塞线程），快请求会被慢请求阻断，RPS 大幅下降

测试结果：
- libco 版：比率 ~97%（混合 26,254 / 基线 26,959），验证 yield 隔离有效
- 无 libco 版：比率大幅下降（慢请求阻塞整个线程）

---

## 八、综合与开放

### Q33：这个项目还有什么可以优化的地方？

**关键词**：链接调度、共享栈、writev、零拷贝

**回答**：

1. **解决协程调度延迟**：将 libco 的 epoll fd 注册到 EventLoop 的主 epoll 中，当协程等待的 fd 就绪时立即唤醒，消除 kPollTimeMs 的轮询延迟
2. **共享栈模式**：从私有栈改为共享栈，万级并发下内存从 ~1.3GB 降至几十 MB
3. **零拷贝响应**：静态文件响应使用 `sendfile()` 或 `mmap()`，避免用户态和内核态之间的数据拷贝
4. **OutputBuffer 写优化**：使用 `writev` 将多个 buffer 一次性写入，减少系统调用
5. **TFO（TCP Fast Open）**：启用 TCP_FASTOPEN 减少短连接握手延迟
6. **完善 HTTP 协议支持**：chunked encoding、gzip、范围请求、WebSocket

### Q34：Reactor 和 Proactor 有什么区别？你的服务器为什么选 Reactor？

**关键词**：同步 + 非阻塞 vs 异步、I/O 通知方式

**回答**：

| | Reactor | Proactor |
|---|---------|----------|
| I/O 方式 | 非阻塞 I/O（就绪通知） | 异步 I/O（完成通知） |
| 流程 | epoll_wait → 数据就绪 → 应用层 read | 调用 aio_read → 内核读完数据 → 通知应用程序 |
| 优点 | 跨平台（epoll/kqueue/poll 都支持） | 减少系统调用次数 |
| 缺点 | 需要非阻塞 + EAGAIN 重试 | Linux AIO 支持不完善（仅文件系统） |

选择 Reactor 的原因：
- Linux 上 epoll 非常成熟，性能优异
- 非阻塞 I/O + 协程 hook 后也能实现异步效果
- Proactor 在 Linux 上使用 AIO 有诸多限制（仅支持 O_DIRECT、不支持 socket）

### Q35：开发过程中用到了哪些调试工具？

**关键词**：GDB、strace、perf、gperftools、Wireshark

**回答**：

- **GDB**：定位段错误和崩溃（如 gethostbyname → inet_ntop 的地址越界）
- **strace**：追踪系统调用，确认 socket 行为、hook 前后的调用差异
- **perf top**：性能热点分析，快速定位 CPU 消耗在哪个函数
- **gperftools**：CPU profiling，可视化性能热点火焰图（cmake 可选集成）
- **Wireshark/tcpdump**：抓包验证 HTTP 协议正确性
- **nm/readelf/objdump**：检查符号导出、重定位类型（排查 TLS 问题）
- **Apache Bench**：性能基准数据和对比验证

### Q36：简历上写 64,188 RPS，具体怎么测的？这个数据可信吗？

**关键词**：测试条件、长连接、公平对比

**回答**：

测试条件（可重复验证）：
- 压测命令：`ab -k -c 200 -n 200000 http://localhost:8080/`
- `-k` 表示 Keep-Alive（长连接），避免短连接的连接建立开销
- 每条路由 GET / 只返回 "Hello, world"，handler 内不执行任何 I/O
- 16 线程 sub-Reactor，并发 200，共 20 万请求
- 服务器预热 1000 请求后才开始正式测试

注意：RPS 是「基于当前硬件条件」的数值——16 核 WSL2 虚拟机。如果在真实的物理机或不同配置下测试，结果会有差异。面试时建议说明测试环境，体现数据真实性。

### Q37：如果让你设计一个百万并发的服务器，现在的架构有什么瓶颈？

**关键词**：C10M、内核开销、DPDK、协程调度

**回答**：

当前架构的瓶颈：

1. **epoll 本身**：100 万 fd 的 epoll_wait 即便是 O(1)，每次返回大量就绪事件的处理也会带来可观的开销
2. **系统调用次数**：每个请求至少涉及 accept、read、write、send 等多次系统调用，百万并发下系统调用本身成为瓶颈
3. **内存开销**：每个连接 Buffer（输入 4KB + 输出 4KB）+ TcpConnection 对象 + 协程栈（128KB），百万连接需要 TB 级内存
4. **上下文切换**：16 线程处理百万连接，线程调度开销显著

改进方向：
- **协程调度优化**：将 libco 的 epoll 集成到主 EventLoop，消除轮询延迟
- **使用 SO_REUSEPORT**：多线程各自 bind 相同端口，内核层面做负载均衡
- **零拷贝网络栈**：调研 DPDK 绕过内核协议栈（但会丧失 POSIX 兼容性）
- **减少 per-connection 资源**：协程共享栈、惰性分配 Buffer

实际生产环境中，nginx 采用「多进程 + epoll」架构处理百万并发，一个 worker 进程处理数万连接是合理的。单机百万并发通常需要 DPDK 或 XDP 等内核 bypass 方案。
