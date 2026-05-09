# libco 协程库入门教程

## 前言

libco 是微信后台大规模使用的 C/C++ 协程库，2013 年至今稳定运行在微信后台的数万台机器上。它最核心的设计思路是：**通过 hook 系统调用 + 自主的事件循环，让同步代码获得异步执行的并发能力**。

本教程面向有一定 C/C++ 基础的初学者，围绕四个核心主题展开：

1. **上下文切换** — 协程怎样"暂停"和"恢复"？
2. **Hook 机制** — 如何拦截系统调用，把阻塞变成非阻塞？
3. **事件循环 + epoll** — 如何调度成千上万的协程？
4. **共享栈内存管理** — 如何用少量内存支持海量连接？三大核心结构体是什么？

---

## 1. 上下文切换：协程的灵魂

### 1.1 什么是协程上下文？

协程的本质是**可以暂停和恢复执行的函数**。暂停时需要保存 CPU 寄存器的状态（称为"上下文"），恢复时需要还原这些寄存器。

libco 用 `coctx_t` 结构体保存上下文：

```c
// libco/coctx.h
struct coctx_t
{
#if defined(__i386__)
    void *regs[ 8 ];     // 32位: 8个寄存器
#else
    void *regs[ 14 ];    // 64位: 14个寄存器
#endif
    size_t ss_size;      // 栈大小
    char *ss_sp;         // 栈底指针
};
```

在 x86-64 下，14 个寄存器按以下顺序排列（见 `coctx.cpp` 的枚举）：

```
regs[0]  = r15    regs[7]  = rdi
regs[1]  = r14    regs[8]  = rsi
regs[2]  = r13    regs[9]  = ret (返回地址)
regs[3]  = r12    regs[10] = rdx
regs[4]  = r9     regs[11] = rcx
regs[5]  = r8     regs[12] = rbx
regs[6]  = rbp    regs[13] = rsp (栈指针)
```

### 1.2 初始化上下文：`coctx_make()`

创建协程时，libco 调用 `coctx_make()` 设置初始上下文：

```c
// libco/coctx.cpp (x86-64 版本)
int coctx_make(coctx_t* ctx, coctx_pfn_t pfn, const void* s, const void* s1)
{
    char* sp = ctx->ss_sp + ctx->ss_size - sizeof(void*);
    sp = (char*)((unsigned long)sp & -16LL);   // 16字节对齐

    memset(ctx->regs, 0, sizeof(ctx->regs));

    void** ret_addr = (void**)(sp);
    *ret_addr = (void*)pfn;   // 栈顶放函数地址

    ctx->regs[kRSP] = sp;         // rsp = 栈指针
    ctx->regs[kRETAddr] = (char*)pfn;  // 返回地址 = 函数地址
    ctx->regs[kRDI] = (char*)s;   // rdi = 第一个参数
    ctx->regs[kRSI] = (char*)s1;  // rsi = 第二个参数
    return 0;
}
```

关键点：
- **栈顶写入函数地址**：当 `coctx_swap` 执行 `ret` 指令时，CPU 会从栈顶弹出这个地址并跳转，从而进入协程函数
- **%rdi / %rsi 传参**：x86-64 调用约定前两个参数通过寄存器传递

### 1.3 上下文切换：`coctx_swap()` 汇编分析

这是 libco 最核心的汇编代码，只有约 40 行：

```asm
; libco/coctx_swap.S (x86-64)
coctx_swap:
    ; === 保存当前协程的上下文到 %rdi (第一个参数) ===
    leaq (%rsp), %rax        ; 获取当前 rsp
    movq %rax, 104(%rdi)     ; regs[13] = rsp
    movq %rbx, 96(%rdi)      ; regs[12] = rbx
    movq %rcx, 88(%rdi)
    movq %rdx, 80(%rdi)
    movq 0(%rax), %rax       ; 获取返回地址 (栈顶的值)
    movq %rax, 72(%rdi)      ; regs[9] = ret addr
    movq %rsi, 64(%rdi)
    movq %rdi, 56(%rdi)
    movq %rbp, 48(%rdi)
    movq %r8, 40(%rdi)
    movq %r9, 32(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 16(%rdi)
    movq %r14, 8(%rdi)
    movq %r15, (%rdi)
    xorq %rax, %rax

    ; === 恢复目标协程的上下文从 %rsi (第二个参数) ===
    movq 48(%rsi), %rbp      ; 恢复 rbp
    movq 104(%rsi), %rsp     ; 恢复 rsp (切换栈！)
    movq (%rsi), %r15
    movq 8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %r9
    movq 40(%rsi), %r8
    movq 56(%rsi), %rdi
    movq 80(%rsi), %rdx
    movq 88(%rsi), %rcx
    movq 96(%rsi), %rbx
    leaq 8(%rsp), %rsp       ; 调整栈指针
    pushq 72(%rsi)           ; 把返回地址压入新栈

    movq 64(%rsi), %rsi      ; 恢复 rsi
    ret                      ; ret = 弹出栈顶地址并跳转！
```

**执行流程**：
1. 把所有 callee-saved 寄存器保存到 `curr->ctx`（%rdi 指向当前协程）
2. 从 `pending->ctx`（%rsi 指向目标协程）恢复所有寄存器
3. 关键一步 `movq 104(%rsi), %rsp` — 切换栈指针，这是协程切换的本质
4. 把目标协程的返回地址压栈后 `ret`，跳转到目标协程上次暂停的地方

```
┌──────────────┐          ┌──────────────┐
│   协程A      │          │   协程B      │
│  (当前运行)  │          │  (待恢复)    │
└──────┬───────┘          └──────┬───────┘
       │                         │
       │  coctx_swap(&A, &B)     │
       ├─────────────────────────┤
       │  保存A的寄存器到 A.ctx   │
       │  从B.ctx恢复B的寄存器   │
       │  切换rsp到B的栈        │
       │  ret → 跳转到B的代码    │
       └─────────────────────────┘
                               │
                               ▼
                          B继续执行
```

**💡 核心要点**：
- `coctx_swap` 不关心协程函数是什么，它只做一件事：保存当前寄存器，加载另一组寄存器
- 切换 `%rsp` 就是切换栈，这是协程切换的本质
- 整个切换过程只涉及几十条指令，没有系统调用，因此比线程切换快得多

---

## 2. Hook 机制：让阻塞变"非阻塞"

### 2.1 为什么需要 Hook？

在普通的同步网络编程中，调用 `read(fd, buf, size)` 时，如果 fd 没有数据可读，线程会**阻塞等待**。但在协程环境下，我们希望的是：**如果 fd 没有数据，就让出 CPU 去执行其他协程，等数据就绪了再回来继续执行**。

libco 通过 hook 系统调用来实现这一目标。

### 2.2 原理：dlsym(RTLD_NEXT)

libco 利用动态链接的 `dlsym(RTLD_NEXT, ...)` 找到 libc 中原始函数的地址并保存：

```c
// libco/co_hook_sys_call.cpp
static read_pfn_t g_sys_read_func = (read_pfn_t)dlsym(RTLD_NEXT, "read");
static write_pfn_t g_sys_write_func = (read_pfn_t)dlsym(RTLD_NEXT, "write");
static poll_pfn_t g_sys_poll_func = (poll_pfn_t)dlsym(RTLD_NEXT, "poll");
// ... 还有 socket, connect, close, send, recv, sendto, recvfrom ...
```

然后定义同名的 `read()`、`write()` 等函数。由于这些函数定义在共享库中，链接时它们会覆盖 libc 的同名函数。

### 2.3 fd 的管理：rpchook_t

libco 需要给每个 socket fd 记录一些元数据：

```c
struct rpchook_t
{
    int user_flag;              // 用户设置的 flag（如 O_NONBLOCK）
    struct sockaddr_in dest;    // 目标地址
    int domain;                 // AF_INET / AF_LOCAL
    struct timeval read_timeout;
    struct timeval write_timeout;
};

static rpchook_t *g_rpchook_socket_fd[102400] = { 0 };
```

`g_rpchook_socket_fd` 是一个大数组，用 fd 做索引。通过 `get_by_fd(fd)` / `alloc_by_fd(fd)` / `free_by_fd(fd)` 管理。

### 2.4 fcntl hook：关键的"欺骗"

为了让系统调用永不阻塞，libco 的 `fcntl` hook 做了一件关键的事：

```c
int fcntl(int fildes, int cmd, ...)
{
    // ...
    switch (cmd)
    {
    case F_SETFL:
    {
        int param = va_arg(arg_list, int);
        int flag = param;
        if (co_is_enable_sys_hook() && lp)
        {
            flag |= O_NONBLOCK;   // 强制加上 O_NONBLOCK！
        }
        ret = g_sys_fcntl_func(fildes, cmd, flag);
        if (0 == ret && lp)
        {
            lp->user_flag = param;  // 保存用户原来的 flag
        }
        break;
    }
    // ...
}
```

这意味着：当用户调用 `fcntl(fd, F_SETFL, 0)` 想设置阻塞 IO 时，libco 实际设置的是 `O_NONBLOCK`，但把用户期望的 flag 保存在 `lp->user_flag` 中。**底层 fd 永远是 non-blocking 的**。

### 2.5 read 和 write hook：阻塞变协程调度

有了 non-blocking fd 作为基础，`read` hook 的模式就很清晰了：

```c
ssize_t read(int fd, void *buf, size_t nbyte)
{
    HOOK_SYS_FUNC(read);

    // 1. 如果没开 hook，直接走系统调用
    if (!co_is_enable_sys_hook())
        return g_sys_read_func(fd, buf, nbyte);

    rpchook_t *lp = get_by_fd(fd);

    // 2. 如果用户明确设了 O_NONBLOCK，直接走系统调用
    if (!lp || (O_NONBLOCK & lp->user_flag))
        return g_sys_read_func(fd, buf, nbyte);

    // 3. 计算超时时间
    int timeout = lp->read_timeout.tv_sec * 1000 + lp->read_timeout.tv_usec / 1000;

    // 4. poll → 注册到 epoll → 让出协程
    struct pollfd pf = {0};
    pf.fd = fd;
    pf.events = POLLIN | POLLERR | POLLHUP;
    int pollret = poll(&pf, 1, timeout);   // ← 这里是 libco 的 poll！

    // 5. 当 poll 返回时，说明数据已就绪（或超时），执行真正的 read
    ssize_t readret = g_sys_read_func(fd, (char*)buf, nbyte);
    return readret;
}
```

这个 `poll` 调用不再是 libc 的 `poll`，而是 libco 的 `poll`，它会：
1. 把 fd 注册到 libco 内部的 epoll 实例
2. 设置超时
3. **让出当前协程**（`co_yield_env`）
4. 等事件就绪或超时后，被事件循环恢复执行

### 2.6 connect hook

`connect` 的 hook 更特殊。在 non-blocking fd 上调用 `connect` 会立即返回 `-1` 和 `errno = EINPROGRESS`（连接正在进行中）。libco 的处理是：

```c
int connect(int fd, const struct sockaddr *address, socklen_t address_len)
{
    // ... 系统调用 ...
    int ret = g_sys_connect_func(fd, address, address_len);

    // 如果返回 EINPROGRESS，等待 POLLOUT 事件
    if (ret < 0 && errno == EINPROGRESS)
    {
        struct pollfd pf = {0};
        pf.fd = fd;
        pf.events = POLLOUT | POLLERR | POLLHUP;

        for (int i = 0; i < 3; i++)  // 最多重试 3 次，每次 25s
        {
            pollret = poll(&pf, 1, 25000);
            if (1 == pollret) break;
        }

        if (pf.revents & POLLOUT)   // 连接成功
        {
            int err = 0;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
            // ...
            return 0;
        }
    }
}
```

### 2.7 setsockopt hook：截获超时设置

```c
int setsockopt(int fd, int level, int option_name,
               const void *option_value, socklen_t option_len)
{
    rpchook_t *lp = get_by_fd(fd);
    if (lp && SOL_SOCKET == level)
    {
        if (SO_RCVTIMEO == option_name)
            memcpy(&lp->read_timeout, val, sizeof(*val));
        else if (SO_SNDTIMEO == option_name)
            memcpy(&lp->write_timeout, val, sizeof(*val));
    }
    return g_sys_setsockopt_func(fd, level, option_name, option_value, option_len);
}
```

这样，用户调用 `setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))` 设置超时时间时，libco 会记录下来，后续 hook 的 `read` / `recv` 会用这个时间作为 poll 的超时参数。

```
用户代码                         libco hook 层                   操作系统
    │                               │                              │
    ├─ read(fd, buf, N) ───────────►│                              │
    │                               ├─ co_is_enable_sys_hook() ───►│ yes
    │                               ├─ get_by_fd(fd) ────────────►│ 有 rpchook
    │                               ├─ poll(fds, 1, timeout) ────►│
    │                               │   ├─ epoll_ctl(ADD) ────────►│
    │                               │   ├─ co_yield() ← 暂停协程  │
    │                               │   │                          │
    │                               │   │    [等待 epoll_wait 返回]
    │                               │   │                          │
    │                               │   ├─ co_resume() → 恢复协程  │
    │                               ├─ g_sys_read_func(fd, buf, N)►│
    │◄──────────────────────────────┤                              │
```

**💡 核心要点**：
- `dlsym(RTLD_NEXT)` 获取原始函数，同名函数覆盖 libc 函数
- 强制 fd 始终为 `O_NONBLOCK`，配合 poll+协程让出来实现"看起来同步，实际异步"
- 每种阻塞操作被拆解为：注册 epoll → 让出协程 → 等待事件 → 恢复 → 执行真正的 IO

---

## 3. 事件循环 + epoll 协同

### 3.1 核心结构体：stCoEpoll_t

```c
struct stCoEpoll_t
{
    int iEpollFd;                             // epoll 实例的 fd
    static const int _EPOLL_SIZE = 1024 * 10;

    struct stTimeout_t *pTimeout;             // 时间轮定时器

    struct stTimeoutItemLink_t *pstTimeoutList;  // 超时事件链表
    struct stTimeoutItemLink_t *pstActiveList;   // 活跃事件链表

    co_epoll_res *result;                     // epoll_wait 结果缓冲区
};
```

每个线程拥有一个 `stCoEpoll_t` 实例（存储在 `stCoRoutineEnv_t` 中）。

### 3.2 时间轮定时器：stTimeout_t

libco 的定时器通过**时间轮**实现，比传统的有序链表更高效：

```c
struct stTimeout_t
{
    stTimeoutItemLink_t *pItems;   // 槽位数组（环形缓冲区）
    int iItemSize;                 // 槽位数（默认 60 * 1000 = 60000）

    unsigned long long ullStart;   // 起始时间（毫秒）
    long long llStartIdx;          // 当前起始槽索引
};

struct stTimeoutItem_t
{
    stTimeoutItem_t *pPrev, *pNext;     // 双向链表指针
    stTimeoutItemLink_t *pLink;         // 所属链表

    unsigned long long ullExpireTime;   // 过期时间（毫秒）
    OnPreparePfn_t pfnPrepare;          // 事件就绪时的预处理函数
    OnProcessPfn_t pfnProcess;          // 事件处理函数
    void *pArg;                         // 参数（通常是协程指针）
    bool bTimeout;                      // 是否超时
};
```

添加定时器：

```c
int AddTimeout(stTimeout_t *apTimeout, stTimeoutItem_t *apItem,
               unsigned long long allNow)
{
    unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart;
    if (diff >= (unsigned long long)apTimeout->iItemSize)
        diff = apTimeout->iItemSize - 1;   // 最多放到最后一个槽

    // 计算槽位索引，放入链表
    int idx = (apTimeout->llStartIdx + diff) % apTimeout->iItemSize;
    AddTail(apTimeout->pItems + idx, apItem);
    return 0;
}
```

取出所有过期的定时器：

```c
inline void TakeAllTimeout(stTimeout_t *apTimeout, unsigned long long allNow,
                           stTimeoutItemLink_t *apResult)
{
    int cnt = allNow - apTimeout->ullStart + 1;
    if (cnt > apTimeout->iItemSize) cnt = apTimeout->iItemSize;

    for (int i = 0; i < cnt; i++)
    {
        int idx = (apTimeout->llStartIdx + i) % apTimeout->iItemSize;
        Join(apResult, apTimeout->pItems + idx);  // 把链表中所有节点移入结果
    }
    apTimeout->ullStart = allNow;
    apTimeout->llStartIdx += cnt - 1;
}
```

时间轮的关键优势：**O(1) 添加，O(1) 取出过期定时器**。传统定时器使用有序链表或 `std::set`，添加和删除都是 O(log n)。

### 3.3 核心循环：co_eventloop()

```
co_eventloop()
    │
    ├─ co_epoll_wait(epfd, events, 10240, 1ms)  ← 1ms 超时
    │
    ├─ for each event:
    │     ├─ pfnPrepare(item, event, activeList)  ← 设置 revents
    │     │   └─ OnPollPreparePfn: 记录 fd 的就绪事件，把 poll 加入 activeList
    │     └─ or: AddTail(activeList, item)          ← 加入活跃列表
    │
    ├─ TakeAllTimeout(pTimeout, now, timeoutList)   ← 取出过期定时器
    │
    ├─ Join(activeList, timeoutList)                ← 合并到活跃列表
    │
    ├─ for each item in activeList:
    │     ├─ PopHead(activeList, item)
    │     ├─ if timeout but not yet expired:        ← 定时器精度修正
    │     │     AddTimeout(pTimeout, item, now)     ← 重新加入
    │     │     continue
    │     ├─ pfnProcess(item)                       ← 处理事件
    │     │   └─ OnPollProcessEvent: co_resume(co)  ← 恢复协程！
    │
    └─ optional: pfn(arg) callback                  ← 用户自定义回调
```

关键点：
- **1ms 超时**: `epoll_wait` 超时设置为 1ms，保证定时器精度在 1ms 级别
- **pfnPrepare**: 当 epoll 返回事件时，调用 `OnPollPreparePfn`，它把 epoll 事件转换为 `pollfd.revents`，然后把 `stPoll_t` 加入 active list
- **pfnProcess**: 当处理 active list 时，调用 `OnPollProcessEvent`，它调用 `co_resume(co)` 恢复等待 IO 的协程

### 3.4 桥接函数：co_poll_inner()

`co_poll_inner()` 是协程和事件循环之间的桥梁。它的参数是一个 `pollfd` 数组和超时时间：

```c
int co_poll_inner(stCoEpoll_t *ctx, struct pollfd fds[], nfds_t nfds,
                  int timeout, poll_pfn_t pollfunc)
{
    if (timeout == 0) return pollfunc(fds, nfds, timeout);  // 不等待

    stCoRoutine_t* self = co_self();
    stPoll_t& arg = *((stPoll_t*)malloc(sizeof(stPoll_t)));
    // ... 初始化 ...

    // 1. 注册所有 fd 到 epoll
    for (nfds_t i = 0; i < nfds; i++)
    {
        arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
        // ... epoll_ctl(ADD, fd, events) ...
    }

    // 2. 添加超时
    arg.ullExpireTime = now + timeout;
    AddTimeout(ctx->pTimeout, &arg, now);

    // 3. 挂起当前协程！
    co_yield_env(co_get_curr_thread_env());

    // 4. 被唤醒后，清理 epoll 注册，复制结果
    // ... epoll_ctl(DEL, fd) ...
    // ... fds[i].revents = arg.fds[i].revents ...
    return iRaiseCnt;
}
```

**关键流程**：

```
用户协程                     epoll 实例              事件循环(另一个栈上)
    │                           │                        │
    ├─ poll(fds, nfds, t) ─────►│                        │
    │   ├─ epoll_ctl(ADD) ─────►│                        │
    │   ├─ AddTimeout ─────────►│ (加入时间轮)            │
    │   ├─ co_yield() ─────────┤                        │
    │   │                      │                        │
    │   │                      │   ┌─ epoll_wait ───────┤
    │   │                      │   │   → 事件就绪!      │
    │   │                      │   ├─ pfnPrepare:       │
    │   │                      │   │   设置 revents     │
    │   │                      │   │   加入 active list │
    │   │                      │   │                    │
    │   │                      │   ├─ pfnProcess:       │
    │   │                      │   │   co_resume(co) ───┤
    │   │◄─ co_resume() ───────┤   │                    │
    │   ├─ epoll_ctl(DEL) ────►│                        │
    │   ├─ 复制 revents        │                        │
    │◄──── 返回 poll 结果 ─────┤                        │
```

**💡 核心要点**：
- `co_eventloop` 是事件循环的"发动机"，持续地执行 `epoll_wait → 分派事件 → 恢复协程`
- `co_poll_inner` 是协程进入等待的"入口"：注册事件 → 让出 → 回来后清理
- 时间轮定时器 O(1) 的添加和过期取出保证了高性能
- Hook 的 `poll()` 调用最终通过 `co_poll_inner` 与事件循环对接

---

## 4. 共享栈与三大核心结构体

### 4.1 三大结构体

#### 结构体一：stCoRoutine_t（协程）

```c
struct stCoRoutine_t
{
    stCoRoutineEnv_t *env;      // 所属的协程环境（每个线程一个）
    pfn_co_routine_t pfn;       // 协程函数
    void *arg;                  // 函数参数
    coctx_t ctx;                // CPU 上下文（寄存器+栈指针）

    char cStart;                // 是否已启动
    char cEnd;                  // 是否已结束
    char cIsMain;               // 是否为主协程
    char cEnableSysHook;        // 是否启用系统调用 hook
    char cIsShareStack;         // 是否使用共享栈

    void *pvEnv;                // 协程本地环境变量（用于 hook env）

    stStackMem_t* stack_mem;    // 栈内存

    // 共享栈相关：被换出时保存栈内容
    char* stack_sp;             // 当前栈指针
    unsigned int save_size;     // 保存的栈大小
    char* save_buffer;          // 保存的栈内容缓冲

    stCoSpec_t aSpec[1024];     // 协程本地存储（类似 thread-local）
};
```

#### 结构体二：stStackMem_t（栈内存块）

```c
struct stStackMem_t
{
    stCoRoutine_t* occupy_co;   // 当前占用此栈的协程
    int stack_size;             // 栈大小
    char* stack_bp;             // 栈顶（高地址）= stack_buffer + stack_size
    char* stack_buffer;         // 栈内存起始（低地址）
};
```

#### 结构体三：stShareStack_t（共享栈池）

```c
struct stShareStack_t
{
    unsigned int alloc_idx;     // 轮询分配索引
    int stack_size;             // 每个栈的大小
    int count;                  // 栈数量
    stStackMem_t** stack_array; // 栈数组
};
```

#### 补充：stCoRoutineEnv_t（线程级环境）

```c
struct stCoRoutineEnv_t
{
    stCoRoutine_t *pCallStack[128];  // 调用栈（最多嵌套 128 层）
    int iCallStackSize;              // 调用栈深度
    stCoEpoll_t *pEpoll;             // epoll 事件循环实例

    // 共享栈相关
    stCoRoutine_t* pending_co;       // 即将运行的协程
    stCoRoutine_t* occupy_co;        // 上次占用当前栈的协程
};
```

### 4.2 内存布局示意

```
私有栈模式：
┌─────────────────────────────┐
│    stCoRoutine_t A          │
│    │                        │
│    └─ stack_mem ───────────►│  stStackMem_t
│                             │    stack_buffer ──► ┌─────────┐
│                             │                      │ A的栈   │ 128KB
│                             │    stack_bp ────────►└─────────┘
│                             │
│    stCoRoutine_t B          │
│    │                        │
│    └─ stack_mem ───────────►│  stStackMem_t
│                                  stack_buffer ──► ┌─────────┐
│                                                    │ B的栈   │ 128KB
│                                  stack_bp ────────►└─────────┘

共享栈模式（N个协程共享M个栈，M < N）：
stShareStack_t
    stack_array[0] ──► stStackMem_t  ──► ┌─────────┐
    stack_array[1] ──► stStackMem_t  ──► │ 物理栈  │ M个物理栈
                                          └─────────┘
    alloc_idx: 轮询选择下一个可用栈

10000个协程 × 128KB = 1.28GB (私有栈)
10000个协程 × 4个共享栈 × 128KB = 512KB + 少量 save_buffer (共享栈)
```

### 4.3 共享栈的切换算法

共享栈的核心在 `co_swap()` 函数中：

```c
void co_swap(stCoRoutine_t* curr, stCoRoutine_t* pending_co)
{
    stCoRoutineEnv_t* env = co_get_curr_thread_env();

    // 1. 记录当前栈指针（通过局部变量的地址）
    char c;
    curr->stack_sp = &c;

    if (!pending_co->cIsShareStack)
    {
        env->pending_co = NULL;
        env->occupy_co = NULL;
    }
    else
    {
        env->pending_co = pending_co;
        // 获取当前占用 pending_co 这个栈内存的协程
        stCoRoutine_t* occupy_co = pending_co->stack_mem->occupy_co;
        // 把 pending_co 标记为占用者
        pending_co->stack_mem->occupy_co = pending_co;
        env->occupy_co = occupy_co;

        if (occupy_co && occupy_co != pending_co)
        {
            // 把原来的占用者的栈内容保存到它的 save_buffer
            save_stack_buffer(occupy_co);
        }
    }

    // 2. 上下文切换
    coctx_swap(&(curr->ctx), &(pending_co->ctx));

    // 3. ⚠️ 切换回来后，栈可能已被覆盖！重新获取数据
    stCoRoutineEnv_t* curr_env = co_get_curr_thread_env();
    stCoRoutine_t* update_occupy_co = curr_env->occupy_co;
    stCoRoutine_t* update_pending_co = curr_env->pending_co;

    if (update_occupy_co && update_pending_co &&
        update_occupy_co != update_pending_co)
    {
        // 把之前被换出的协程的栈内容恢复回来
        if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
        {
            memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer,
                   update_pending_co->save_size);
        }
    }
}
```

`save_stack_buffer` 的实现：

```c
void save_stack_buffer(stCoRoutine_t* occupy_co)
{
    stStackMem_t* stack_mem = occupy_co->stack_mem;
    int len = stack_mem->stack_bp - occupy_co->stack_sp;
    // stack_bp 是栈顶（高地址），stack_sp 是当前栈指针
    // len 是当前栈上有效数据的长度

    // 释放旧的 save_buffer，分配新的
    if (occupy_co->save_buffer)
        free(occupy_co->save_buffer);

    occupy_co->save_buffer = (char*)malloc(len);
    occupy_co->save_size = len;

    // 把栈上数据从当前指针到栈顶全部复制到 save_buffer
    memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}
```

### 4.4 共享栈的完整生命周期

```
时间点   事件                             物理栈 P0 的内容    协程 A 的状态
─────────────────────────────────────────────────────────────────────────
T1     A 运行中                          [A 的数据]           running, occupy=P0
T2     A: poll() → co_yield()            [A 的数据]           saved sp, 暂停
T3     B 被调度，B 也使用这个栈            [B 的数据]           - 
       co_swap(A→B):
         - 发现 P0.occupy_co == A
         - stack_bp - stack_sp = 64KB
         - malloc + memcpy → A.save_buffer
         - P0.occupy_co = B
         - coctx_swap → B 开始运行        [B 的数据]
T4     B: co_yield()                     [B 的数据]           -
       co_swap(B→A):
         - 发现 P0.occupy_co == B
         - save_stack_buffer(B)           [B 的数据 → B.save_buffer]
         - P0.occupy_co = A
         - coctx_swap → A 恢复
         - 发现 A.save_buffer 存在
         - memcpy(A.save_buffer → A.stack_sp)
                                          [A 的数据]           A 恢复运行！
```

**关键洞察**：共享栈的"共享"在于多个协程轮流使用同一个物理栈。切出时把栈内容保存到堆上（`save_buffer`），切入时如果换入的协程之前被保存过，再把内容从堆上复制回来。这是一种**时间换空间**的策略。

### 4.5 私有栈 vs 共享栈

| 特性 | 私有栈 | 共享栈 |
|------|--------|--------|
| 内存占用 | 每个协程独占 128KB | 多个协程共享几个 128KB 栈 |
| 切换开销 | 无额外内存操作 | 需要 memcpy 保存/恢复栈内容 |
| 适用场景 | 协程数量少（几百） | 海量连接（数万到数百万） |
| 线程安全 | 天然隔离 | save/restore 需要谨慎 |
| 使用方式 | `attr.share_stack = NULL` | `attr.share_stack = co_alloc_sharestack(count, size)` |

**💡 核心要点**：
- `stCoRoutine_t` 描述"协程"，`stStackMem_t` 描述"栈内存"，`stShareStack_t` 描述"共享栈池"
- 共享栈的切换在 `co_swap()` 中自动完成：切出时保存原占用者的栈，切入时恢复目标协程的栈
- 实现细节中有两层保护：`occupy_co` 追踪栈的当前使用者，`save_buffer` 保存被换出者的栈内容
- `co_swap` 返回后需要**重新获取** `env` 指针，因为栈可能已被其他协程覆盖

---

## 5. 串联起来：一条 read 请求的完整旅程

让我们追踪一条 `read(fd, buf, N)` 调用在 libco 中的完整路径：

```
步骤 1: 应用代码调用 read(fd, buf, N)
          │
          ▼
步骤 2: libco 的 hooked read() (co_hook_sys_call.cpp)
          ├─ co_is_enable_sys_hook() → true
          ├─ get_by_fd(fd) → 有 rpchook_t
          ├─ user_flag 无 O_NONBLOCK → 需要等待
          ├─ 计算超时时间 (来自 rpchook_t.read_timeout)
          └─ 调用 poll(&pf, 1, timeout)
                    │
                    ▼
步骤 3: libco 的 hooked poll() (co_hook_sys_call.cpp)
          ├─ co_is_enable_sys_hook() → true
          └─ 调用 co_poll_inner(ctx, fds, nfds, timeout, sys_poll)
                    │
                    ▼
步骤 4: co_poll_inner (co_routine.cpp) — 桥接层
          ├─ 分配 stPoll_t + stPollItem_t
          ├─ for each fd:
          │     ├─ PollEvent2Epoll() 转换事件类型
          │     └─ epoll_ctl(EPOLL_CTL_ADD, fd, &ev)  ← 上下文切换①: 进入协程
          │
          ├─ AddTimeout(pTimeout, &arg, now)  ← 加入时间轮
          │
          ├─ co_yield_env(env)  ← 上下文切换②: 让出协程
          │     ├─ 把当前协程从调用栈弹出
          │     ├─ co_swap(curr, last)
          │     │     ├─ 保存 curr 的寄存器到 curr.ctx
          │     │     ├─ 处理共享栈 save/restore
          │     │     └─ coctx_swap → 切换到事件循环所在协程
          │     │
          │     ▼  [协程挂起，事件循环继续运行]
          │
步骤 5: co_eventloop (co_routine.cpp) — 事件循环
          │
          ├─ [循环开始]
          │
          ├─ co_epoll_wait(epfd, result, 10240, 1ms)
          │     ├─ 内核检测到 fd 有数据可读
          │     └─ 返回就绪事件列表
          │
          ├─ for each ready event:
          │     └─ pfnPrepare = OnPollPreparePfn
          │           ├─ 设置 pSelf->revents = POLLIN
          │           ├─ pPoll->iRaiseCnt++
          │           └─ AddTail(active, pPoll)
          │
          ├─ TakeAllTimeout(pTimeout, now, timeoutList)
          │
          ├─ Join(activeList, timeoutList)
          │
          ├─ for each item in activeList:
          │     └─ pfnProcess = OnPollProcessEvent
          │           └─ co_resume(co)  ← 上下文切换③: 恢复协程
          │                 ├─ co_swap(curr, pending)
          │                 │     ├─ 保存事件循环上下文
          │                 │     ├─ 处理共享栈 restore
          │                 │     └─ coctx_swap → 回到协程
          │                 │
          │                 ▼  [协程恢复]
          │
步骤 6: co_poll_inner (从 co_yield_env 之后继续)
          ├─ 清理:
          │     ├─ RemoveFromLink(pTimeout)
          │     ├─ epoll_ctl(DEL, fd) 每个 fd
          │     └─ 复制 revents 结果
          └─ 返回 iRaiseCnt
                    │
                    ▼
步骤 7: hooked read() (从 poll 之后继续)
          └─ g_sys_read_func(fd, buf, N)  ← 此时数据已就绪，立即返回
                    │
                    ▼
步骤 8: 应用代码获得数据

总共涉及的机制:
  ├─ Hook: 步骤 2-3 (dlsym + 函数覆盖)
  ├─ Epoll: 步骤 4 (epoll_ctl), 步骤 5 (epoll_wait)
  ├─ 时间轮: 步骤 4 (AddTimeout), 步骤 5 (TakeAllTimeout)
  ├─ 协程切换: 步骤 4 (co_yield), 步骤 5 (co_resume)
  └─ 共享栈: 步骤 4 (co_swap 内的 save/restore)
```

## 6. 如何在 kama-webserver 中集成 libco

### 6.1 构建集成

libco 已有独立的 `CMakeLists.txt`，在项目根 `CMakeLists.txt` 中添加：

```cmake
add_subdirectory(libco)
target_link_libraries(main co)  # 或其他目标
```

### 6.2 基本使用模式

```c
#include "co_routine.h"

// 1. 创建协程
stCoRoutine_t *co = NULL;
co_create(&co, NULL, my_routine_func, arg);

// 2. 恢复执行
co_resume(co);

// 3. 在协程内部主动让出
co_yield_ct();  // 让出当前协程

// 4. 启用系统调用 hook
co_enable_hook_sys();

// 5. 事件循环（通常在独立的线程中运行）
co_eventloop(co_get_epoll_ct(), NULL, NULL);
```

### 6.3 与 webserver 结合思路

kama-webserver 现有架构是 Reactor 模式（main loop accept + sub loops 处理 IO）。libco 可以改造为：

- **每个 sub loop 线程跑一个 `co_eventloop`**，替代现有的 epoll_wait 循环
- **每个 HTTP 请求在一个协程中处理**，通过 `co_poll` 等待 IO
- **启用 hook**，使现有的 `read/write/send/recv` 调用自动变为非阻塞等待

### 参考示例

libco 目录下提供了多个示例可参考：
- `example_echosvr.cpp` — echo 服务器
- `example_echocli.cpp` — echo 客户端
- `example_poll.cpp` — poll 使用示例
- `example_copystack.cpp` — 共享栈示例

---

## 总结

| 主题 | 核心文件 | 一句话总结 |
|------|---------|-----------|
| 上下文切换 | `coctx_swap.S`, `coctx.cpp` | 保存/恢复 14 个寄存器 + 切换 %rsp |
| Hook 机制 | `co_hook_sys_call.cpp` | dlsym 获取原始函数 + 强制 O_NONBLOCK + poll 等待 |
| 事件循环 | `co_routine.cpp`, `co_epoll.cpp` | epoll_wait + 时间轮定时器 + 分派回调恢复协程 |
| 共享栈 | `co_routine_inner.h`, `co_routine.cpp` | 切出时 memcpy 保存栈，切入时 memcpy 恢复 |
