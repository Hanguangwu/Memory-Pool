# 从 0 到 1 手写 C++ 内存池：固定预分配版 + 自动扩容版两种实现详解

> 关键词：C++、内存池、memory pool、operator new、性能优化、面试
> 适合人群：想搞懂内存池原理、准备 C++ 后端/游戏开发面试、需要在高频小对象场景做性能优化的工程师
> 阅读时间：约 15 分钟

---

## 开篇：为什么我们需要内存池？

先看一个非常典型的性能问题。

在 C++ 服务端或游戏引擎里，我们经常写出这样的代码：

```cpp
for (int i = 0; i < 1000000; ++i) {
    Msg* msg = new Msg(...);   // 一次 new
    process(msg);
    delete msg;                // 一次 delete
}
```

看起来没毛病，但 `new/delete` 在底层其实是在调用 `malloc/free`，这背后至少干了三件事：

1. **用户态 → 内核态切换**，陷入系统调用；
2. **遍历空闲链表**寻找合适大小的块；
3. **维护堆元数据**，合并/拆分空闲块。

在每秒百万级小对象创建销毁的场景下，这三件事的开销会被放大到非常夸张的程度，还会留下大量**内存碎片**——明明总内存还够用，却因为没有一块连续的大块而分配失败。

**内存池（Memory Pool）就是为了解决这个问题而生的。**

它的核心思想一句话：

> 自己向操作系统一次性要一大块内存（Chunk），切成若干固定大小的小格子（Block），以后 `new` 就从格子里拿一个，`delete` 就把格子挂回去，全程不再找操作系统。

---

## 一、内存池核心概念：Chunk 与 Block

在动手写代码之前，先把两个关键概念对齐，后面所有实现都围绕它们展开。

| 术语 | 含义 | 类比 |
|------|------|------|
| **Chunk（大块内存）** | 内存池向操作系统一次性申请的连续内存区域，是内存池的"进货单位" | 批发进来的一整箱货 |
| **Block（对象块）** | Chunk 内部被切分出来的最小分配单元，大小通常对齐到业务对象大小 | 箱子里一个个独立的小包装盒 |
| **FreeList（空闲链表）** | 把所有空闲 Block 串成的单向链表，分配时摘头，释放时插头 | 空盒子的台账 |

整个内存池的数据结构长这样：

```
┌─────────────── Chunk 1 ────────────────┐
│ ChunkHeader │ Block0 │ Block1 │ Block2 │ ... │
└─────────────┴────────┴────────┴────────┴─────┘
                    │        ↑
                    └────────┘  Block 内部空闲时存下一个 Block 地址
                              (FreeList 链表)
```

一个非常巧妙的设计是：**空闲 Block 不需要额外的指针空间**，直接利用 Block 自己的前 8 个字节存下一个空闲 Block 的地址。这样除了 ChunkHeader 那一点点元数据，分配对象几乎零开销。

---

## 二、内存池的优缺点与适用场景

### ✅ 优势

1. **大幅减少系统调用**：从每对象一次 `malloc` 变成每 N 个对象一次大块申请；
2. **消除内存碎片**：所有 Block 大小一致，归还后立刻可复用，不会产生"你一块我一块"的细碎碎片；
3. **分配/释放 O(1)**：摘头插头，不遍历、不合并、不找空位；
4. **耗时稳定**：没有系统调用抖动，适合游戏帧循环、实时控制系统这类对延迟敏感的场景。

### ❌ 代价

1. **预占内存**：哪怕一个对象都没 new，Chunk 已经占住了；
2. **实现复杂度**：比直接 `malloc` 多了对齐、链表、扩容、析构释放等逻辑；
3. **固定块大小**：经典实现只适合同尺寸对象，不适合大块/变长对象；
4. **线程安全要自己加锁**：FreeList 是非线程安全的，多线程环境必须包 `std::mutex`。

### 🎯 典型适用场景

- 游戏：粒子、子弹、NPC、UI 元素等海量小对象；
- 网络：消息报文、连接对象、缓冲区；
- 高性能服务器：数据库/Session/请求对象；
- 嵌入式/实时系统：需要确定的分配延迟。

---

## 三、实现一：FixedMemPool —— 单 Chunk 固定预分配版

先从最简单的版本开始：**一次性向系统要一块内存，用完就没了**。这个版本最适合用来理解内存池的本质。

### 3.1 完整代码

```cpp
#include <iostream>
#include <cstring>

// 单 Chunk 固定预分配内存池
class FixedMemPool {
public:
    /**
     * @param blockSize 单个 Block 的字节大小
     * @param blockNum  预分配 Block 总数
     */
    FixedMemPool(size_t blockSize, size_t blockNum)
        : m_blockSize(blockSize), m_blockNum(blockNum)
    {
        // ① 一次性向操作系统申请一整块连续内存（Chunk）
        m_chunkStart = static_cast<char*>(::operator new(blockSize * blockNum));
        m_freeList   = m_chunkStart;

        // ② 把所有 Block 串成单向空闲链表
        char* p = m_chunkStart;
        for (size_t i = 0; i < blockNum - 1; ++i) {
            // 每个 Block 的前 8 字节，存下一个空闲 Block 的地址
            *reinterpret_cast<char**>(p) = p + blockSize;
            p += blockSize;
        }
        *reinterpret_cast<char**>(p) = nullptr;   // 最后一个 Block 尾巴置空
    }

    ~FixedMemPool() {
        // ③ 析构时整块释放，一次性还给操作系统
        ::operator delete(m_chunkStart);
    }

    // 禁止拷贝，内存池不能被复制
    FixedMemPool(const FixedMemPool&)            = delete;
    FixedMemPool& operator=(const FixedMemPool&) = delete;

    // 从空闲链表头摘一个 Block
    void* allocate() {
        if (m_freeList == nullptr) {
            std::cerr << "[FixedMemPool] 内存池已耗尽！\n";
            return nullptr;
        }
        char* ret    = m_freeList;
        m_freeList   = *reinterpret_cast<char**>(m_freeList);
        return ret;
    }

    // 把 Block 挂回空闲链表头
    void deallocate(void* ptr) {
        if (ptr == nullptr) return;
        char* p         = static_cast<char*>(ptr);
        *reinterpret_cast<char**>(p) = m_freeList;
        m_freeList      = p;
    }

private:
    char*  m_chunkStart;   // Chunk 起始地址
    char*  m_freeList;     // 空闲 Block 链表头
    size_t m_blockSize;    // 单个 Block 大小
    size_t m_blockNum;     // Block 总数
};
```

### 3.2 测试：用 placement new 在 Block 上构造对象

这个版本**不重载 operator new**，所以需要用 placement new 手动在拿到的 Block 上调用构造函数：

```cpp
struct TestObj {
    int    a;
    double b;
    TestObj(int _a, double _b) : a(_a), b(_b) {
        std::cout << "构造 TestObj  a=" << a << ", b=" << b << "\n";
    }
    ~TestObj() {
        std::cout << "析构 TestObj  a=" << a << ", b=" << b << "\n";
    }
};

int main() {
    // Block 大小至少要能放下 TestObj，还要能存下链表指针
    size_t blockSize = sizeof(TestObj) > sizeof(char*) ? sizeof(TestObj)
                                                      : sizeof(char*);
    FixedMemPool pool(blockSize, 5);
    std::cout << "===== FixedMemPool 测试开始 =====\n";

    // ① allocate() 拿到一块原始内存
    // ② placement new 在这块内存上调用构造函数
    TestObj* o1 = new (pool.allocate()) TestObj(1, 1.1);
    TestObj* o2 = new (pool.allocate()) TestObj(2, 2.2);

    // 释放：必须先手动调用析构函数，再把 Block 归还
    o1->~TestObj();
    pool.deallocate(o1);
    o2->~TestObj();
    pool.deallocate(o2);

    return 0;
}
```

### 3.3 运行结果

```
===== FixedMemPool 测试开始 =====
构造 TestObj  a=1, b=1.1
构造 TestObj  a=2, b=2.2
析构 TestObj  a=1, b=1.1
析构 TestObj  a=2, b=2.2
```

### 3.4 这个版本的局限

- 只有 **1 个 Chunk**，Block 用完就 `nullptr`，不能扩容；
- 使用起来啰嗦：必须手动 `new (addr) T()` + `obj->~T()`；
- 没有任何越界/重复释放保护。

---

## 四、实现二：ExpandableMemPool —— 多 Chunk 自动扩容版

工程上更常见的是**自动扩容版**：Block 用光了就再向系统要一个新 Chunk，并且把它切成新的 Block 串进 FreeList。

更进一步，我们直接重载 `TestObj` 的 `operator new / operator delete`，让业务代码写 `new TestObj()` 时**自动走内存池**，一行 placement new 都不用写。

### 4.1 完整代码

```cpp
#include <iostream>
#include <cstdlib>

// 多 Chunk 可扩容内存池
class ExpandableMemPool {
public:
    /**
     * @param blockSize 单个 Block 大小
     * @param expandCnt 每次新增 Chunk 时切出多少个 Block
     */
    ExpandableMemPool(size_t blockSize, size_t expandCnt)
        : m_blockSize(blockSize), m_expandCnt(expandCnt),
          m_freeList(nullptr), m_chunkList(nullptr)
    {
        expandChunk();   // 初始化就申请第一个 Chunk
    }

    ~ExpandableMemPool() {
        // 遍历所有 Chunk，整块释放
        ChunkHeader* cur = m_chunkList;
        while (cur) {
            ChunkHeader* next = cur->nextChunk;
            ::operator delete(cur);
            cur = next;
        }
    }

    void* allocate() {
        // 空闲 Block 用光 → 自动申请新 Chunk
        if (m_freeList == nullptr) expandChunk();

        char* ret   = m_freeList;
        m_freeList  = *reinterpret_cast<char**>(m_freeList);
        return ret;
    }

    void deallocate(void* ptr) {
        if (!ptr) return;
        char* p       = static_cast<char*>(ptr);
        *reinterpret_cast<char**>(p) = m_freeList;
        m_freeList    = p;
    }

    ExpandableMemPool(const ExpandableMemPool&)            = delete;
    ExpandableMemPool& operator=(const ExpandableMemPool&) = delete;

private:
    // 每个 Chunk 头部记录下一个 Chunk 的地址，便于析构时整体释放
    struct ChunkHeader {
        ChunkHeader* nextChunk;
    };

    // 申请一个新 Chunk，切成 m_expandCnt 个 Block，串到 FreeList 头部
    void expandChunk() {
        size_t total = sizeof(ChunkHeader) + m_expandCnt * m_blockSize;
        ChunkHeader* chunk = static_cast<ChunkHeader*>(::operator new(total));
        chunk->nextChunk   = m_chunkList;
        m_chunkList        = chunk;

        char* start = reinterpret_cast<char*>(chunk + 1);   // 跳过头部
        char* p     = start;
        for (size_t i = 0; i < m_expandCnt - 1; ++i) {
            *reinterpret_cast<char**>(p) = p + m_blockSize;
            p += m_blockSize;
        }
        *reinterpret_cast<char**>(p) = m_freeList;
        m_freeList = start;

        std::cout << "[扩容] 新建一个 Chunk，包含 " << m_expandCnt << " 个 Block\n";
    }

private:
    char*        m_freeList;    // 空闲 Block 链表头
    ChunkHeader* m_chunkList;   // 所有 Chunk 的链表
    size_t       m_blockSize;
    size_t       m_expandCnt;
};
```

### 4.2 让业务类自动走内存池：重载 operator new

```cpp
struct TestObj {
    int    a;
    double b;

    TestObj(int _a, double _b) : a(_a), b(_b) {
        std::cout << "构造 TestObj  a=" << a << ", b=" << b << "\n";
    }
    ~TestObj() {
        std::cout << "析构 TestObj  a=" << a << ", b=" << b << "\n";
    }

    // 全局只存在一个内存池，所有 TestObj 共用
    static ExpandableMemPool s_pool;

    // 重载 operator new：new TestObj() 时自动调用这里
    static void* operator new(size_t size) {
        if (size != sizeof(TestObj)) throw std::bad_alloc();
        return s_pool.allocate();
    }
    // 重载 operator delete：delete p 时自动调用这里
    static void  operator delete(void* ptr) noexcept {
        s_pool.deallocate(ptr);
    }
};

// 静态成员必须在类外初始化：Block=sizeof(TestObj)，每次扩容 5 个
ExpandableMemPool TestObj::s_pool(sizeof(TestObj), 5);
```

### 4.3 业务代码：跟普通 new 一模一样

```cpp
int main() {
    std::cout << "===== ExpandableMemPool 测试开始 =====\n";

    TestObj* p1 = new TestObj(1, 1.1);
    TestObj* p2 = new TestObj(2, 2.2);
    TestObj* p3 = new TestObj(3, 3.3);
    TestObj* p4 = new TestObj(4, 4.4);
    TestObj* p5 = new TestObj(5, 5.5);
    TestObj* p6 = new TestObj(6, 6.6);   // ← 这里会自动触发扩容

    std::cout << "\n-- delete 前三个，Block 归还 --\n";
    delete p1;
    delete p2;
    delete p3;

    std::cout << "\n-- 复用归还的 Block 再次 new --\n";
    TestObj* p7 = new TestObj(7, 7.7);
    delete p7;

    std::cout << "\n-- 释放剩余对象 --\n";
    delete p4;
    delete p5;
    delete p6;
    return 0;
}
```

### 4.4 运行结果

```
[扩容] 新建一个 Chunk，包含 5 个 Block
===== ExpandableMemPool 测试开始 =====
构造 TestObj  a=1, b=1.1
构造 TestObj  a=2, b=2.2
构造 TestObj  a=3, b=3.3
构造 TestObj  a=4, b=4.4
构造 TestObj  a=5, b=5.5
[扩容] 新建一个 Chunk，包含 5 个 Block
构造 TestObj  a=6, b=6.6

-- delete 前三个，Block 归还 --
析构 TestObj  a=1, b=1.1
析构 TestObj  a=2, b=2.2
析构 TestObj  a=3, b=3.3

-- 复用归还的 Block 再次 new --
构造 TestObj  a=7, b=7.7
析构 TestObj  a=7, b=7.7

-- 释放剩余对象 --
析构 TestObj  a=4, b=4.4
析构 TestObj  a=5, b=5.5
析构 TestObj  a=6, b=6.6
```

注意看：第 6 个 `new TestObj` 时，第一个 Chunk 的 5 个 Block 刚好用完，内存池**自动新建了第二个 Chunk**。

---

## 五、两种实现对比

| 对比维度 | FixedMemPool（单 Chunk） | ExpandableMemPool（多 Chunk） |
|----------|--------------------------|-------------------------------|
| Chunk 数量 | 1 个，固定不变 | 多个，按需追加 |
| 扩容能力 | ❌ 用完返回 nullptr | ✅ 自动新建 Chunk |
| 业务侧写法 | 必须 placement new + 手动析构 | 重载 operator new，直接 `new T()` |
| 适用场景 | 数量上限已知、资源受限的嵌入式环境 | 数量不确定的服务端/游戏高频小对象 |
| 代码复杂度 | 极简，约 60 行 | 中等，约 120 行 |
| 学习价值 | 理解内存池本质 | 贴近工程实践 |

---

## 六、面试高频追问（建议背下来）

**Q1：内存池为什么能提升性能？**
A：三点——① 减少系统调用，避免用户态/内核态切换；② 消除小块分配产生的堆碎片；③ 分配释放都是 O(1) 的链表操作，耗时稳定。

**Q2：空闲 Block 为什么不另外开个 map 记录？**
A：利用 Block 自身的前 8 字节存下一个空闲块指针，**复用已分配内存做元数据**，零额外开销，这是 SGI STL 二级空间配置器的经典做法。

**Q3：为什么要区分 Chunk 和 Block？**
A：Chunk 是"进货单位"，决定了向 OS 要多少内存；Block 是"售卖单位"，决定了用户每次拿到多大。两者解耦才能在 Block 耗尽时只追加 Chunk，而不影响已分配对象。

**Q4：这个实现有什么问题？**
A：① 只支持固定大小 Block，不能分配任意尺寸；② 非线程安全，多线程要加 `std::mutex`；③ 没有重复释放/越界校验；④ 析构时如果还有对象没 delete 会内存泄漏，需要额外的调试钩子。

**Q5：工业级内存池还会做什么？**
A：多尺寸分级（类似 SGI 的 16 级自由链表）、Thread Cache 线程本地缓存、批量归还、内存对齐对齐到 cache line 减少伪共享、stats 统计等。

---

## 七、编译运行

```bash
# 固定预分配版
g++ fixed_pool.cpp  -o fixed_pool  -std=c++11 -Wall
./fixed_pool

# 自动扩容版
g++ expand_pool.cpp -o expand_pool -std=c++11 -Wall
./expand_pool
```

---

## 写在最后

内存池看起来是个"面试八股"，但它的思想贯穿了整个 C++ 性能优化生态：

- STL 的 `std::allocator` 背后就是分级内存池；
- jemalloc / tcmalloc 是更强壮的多尺寸内存池；
- 游戏引擎里的 `ObjectPool` 几乎都是这个套路。

理解了 Chunk + Block + FreeList 这三件套，再去看任何工业级内存池源码，你会发现骨架都差不多——只是多了线程缓存、大小分级、统计监控这些"装修"。

**觉得有用的话，欢迎收藏转发，面试前拿出来过一遍。**

---

> 附：本文完整代码已拆分到 `fixed_pool.cpp` 和 `expand_pool.cpp`，可直接编译运行。
