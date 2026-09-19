# Kama MemoryPool（三层内存池）

纯 C++11 实现的三层内存池：**ThreadCache → CentralCache → PageCache**，针对多线程高并发小对象（≤256KB）分配/释放场景设计。命名约定统一：大块内存区称 **Chunk**，小块（分配单元）称 **Block**。

本版本相对早期版本的核心差异是**全链路批量化**：ThreadCache 向 CentralCache **批量申请**（一次领一整批 Block，而非按需逐块）；PageCache 向操作系统**批量预取**（一次申请 128 页，切出请求部分后其余缓存复用）。两个行为都有确定性单元测试背书（见 §5）。

---

## 1. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│  调用方  MemoryPool::allocate/deallocate(ptr, size)         │
└──────────────────────────────┬──────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────┐
│ Layer 1  ThreadCache（线程缓存）· thread_local 单例，无锁    │
│   · 每线程每大小类一条空闲 Block 链表（freeList_ + 计数）    │
│   · 命中直接弹出（无锁 O(1)）                                │
│   · 空则向 CentralCache 批量申请一整批（batchSize 个），     │
│     而非按需申请 1 个：第一个返回，其余挂入本地链表，         │
│     此后多次分配不再触碰中央锁                               │
│   · 超过保留阈值则把链表尾部多余 Block 归还 CentralCache     │
└──────────────────────────────┬──────────────────────────────┘
                               │ 批量申请 / 批量归还（每大小类一把自旋锁）
┌──────────────────────────────▼──────────────────────────────┐
│ Layer 2  CentralCache（中心缓存）· 每大小类一把自旋锁        │
│   · 中央空闲 Block 链表，跨线程共享                          │
│   · 链表空时新建 Chunk 并一次性按整批数量发放                │
│   · 块的归属查询走本类「活跃 Chunk 注册表」线性扫描          │
│   · Chunk 的全部 Block 都回笼时，整块归还 PageCache          │
└──────────────────────────────┬──────────────────────────────┘
                               │ 申请/归还整块 Chunk（一把全局互斥锁）
┌──────────────────────────────▼──────────────────────────────┐
│ Layer 3  PageCache（页缓存）· 一把互斥锁                     │
│   · 按页数管理空闲 Chunk（页数 -> 有序集合，lower_bound）     │
│   · 偏大的 Chunk 切割复用；归还时与左右空闲邻居合并          │
│   · 批量预取：空闲列表无合适 Chunk 时，一次向 OS 申请         │
│     PREFETCH_PAGES(128) 页 = 512KB，切出请求部分后其余作为   │
│     空闲 Chunk 缓存，后续分配零系统调用                       │
│   · chunkMap_：起始地址 -> Chunk，O(log n) 邻接判定          │
│   · systemAlloc：Windows VirtualAlloc / POSIX mmap，跨平台  │
└─────────────────────────────────────────────────────────────┘
```

分配路径（快速）：`MemoryPool::allocate` → ThreadCache 链表弹出（无锁）。
分配路径（慢速）：ThreadCache 空 → **批量申请** CentralCache → CentralCache 空 → PageCache **批量预取**。
释放路径：ThreadCache 压栈；超过阈值 → 归还 CentralCache；整块空闲 → 归还 PageCache。

大小对象分流：`size > MAX_BYTES(256KB)` 直接走 `malloc/free`。大小类共 32768 个（8 的倍数），`SizeClass::getIndex` 直接向下取整到对齐单元，分配/释放可传入原始字节数（`getIndex(x) == getIndex(roundUp(x))`）。

---

## 2. 命名约定

| 术语  | 含义                                             | 典型例子                          |
|-------|--------------------------------------------------|-----------------------------------|
| Chunk | 大块内存区：PageCache 从 OS 申请的多页连续内存    | `Chunk` 结构体、`allocateChunk`   |
| Block | 小块：Chunk 切割出的分配单元，以 `void*` 表示     | `fetchBlocks`、`returnBlocks`     |
| 大小类 | 按块大小划分的桶，索引即 `块大小/8 - 1`           | `freeList_[index]`                |

---

## 3. 关键设计点

### 3.1 内嵌 next 指针（无额外元数据开销）
Block 自身前 `sizeof(void*)` 字节存放下一块地址，空闲链表不占用额外内存。
代价：Block 必须是 "至少对齐单元" 且地址为 8 的倍数（天然满足），每个 Block 实际可用字节比请求字节略大（8 的倍数向上取整）。

### 3.2 Chunk 结构与生命周期
```cpp
struct Chunk {
    void*  addr;          // 起始地址（页对齐）
    size_t pageCount;     // 页数
    size_t blockSize;     // 当前切割的块大小（CentralCache 切割时写入）
    size_t totalBlocks;   // 切割出的块总数
    size_t freeInCentral; // 当前位于中央空闲链表中的块数
    bool   inFreeList;    // 是否位于 PageCache 空闲列表（true 时 CentralCache 不得引用）
};
```
生命周期状态机：
```
PageCache 空闲(inFreeList=true) → 切给 CentralCache(inFreeList=false)
  → 全部 Block 回笼 → 整块归还 PageCache → 合并/复用
```

### 3.3 记账不变量
**invariant：`chunk->freeInCentral` 恒等于该 Chunk 当前位于中央空闲链表中的块数。**
- 取块：`--freeInCentral`；归还：`++freeInCentral`；
- `freeInCentral == totalBlocks` ⇒ 整块空闲 ⇒ 可安全整块回收。

记账正确性依赖块归属判定。空闲链表按 next 指针连接，遍历顺序不代表地址连续（归还的块来自任意 Chunk、地址任意交错）。因此归属查询缓存必须按「地址范围」双端校验（`blockAddr ∈ [chunkStart, chunkEnd)`），不在范围内一律重查（`findChunkInRegistry`），绝不可按"遍历连续性"假设。
注册表只登记「活跃 Chunk」（通常每大小类 1~3 个，线性扫描 O(1)~O(3)），全程在自旋锁内完成，**不在分配热路径上触碰 PageCache 的全局互斥锁**（这是性能关键的优化）。

### 3.4 批量化：ThreadCache 批量申请，而非按需申请
- **批量申请**：ThreadCache 慢速路径一次向 CentralCache 领取一整批
  `batchSize = clamp(512/blockSize, 2, 64)` 个 Block：小块多取摊薄锁开销，大块少取避免驻留。
  第一个 Block 直接返回给调用者，其余挂入线程本地链表，**后续分配不再触碰中央每类自旋锁**。
- **批量保证**：中央链表空时，`fetchBlocks` 新建 Chunk 并按满批数量发放（见
  `testCentralCacheBatchFetch`：清空链表后一次 `fetchBlocks(index, 64)` 恰好返回 64 个）。
- 线程内每大小类保留上限 `keepLimit = max(batchSize*2, 32)`：超过即归还尾部多余段（最近最少复用，保持局部性）。
- 所有链表长度计数均为精确维护（弹出减、压入加、批量领取按实际块数加），**无回绕**；
  `fetchFromCentral` 自行遍历统计实际块数，不信任中央返回值（防御式）。

### 3.5 整块回收时机与陈旧条目防护
回收条件 `freeInCentral == totalBlocks` 蕴含一个重要性质：**没有任何线程仍持有该 Chunk 的 Block**（全部已回笼中央链表）。因此在回收临界区内：
1. 该 Chunk 的所有 Block 从中央链表摘除；
2. 从注册表摘除（此后归属查询不再命中）；
3. 归还 PageCache；PageCache 依据 `inFreeList` 决定能否合并删除。

任何仍持有该 Chunk 地址的线程都不存在 → 杜绝了旧实现"陈旧条目二次归还导致 use-after-free"的路径。

### 3.6 PageCache 不变量与合并
```
I1: chunkMap_ 覆盖所有现存 Chunk（含空闲与已分配）。空闲 Chunk 也登记，合并才能找到邻居。
I2: freeChunks_ 恰好包含所有 inFreeList == true 的 Chunk。
I3: CentralCache 只引用 inFreeList == false 的 Chunk；PageCache 只合并/删除
    inFreeList == true 的 Chunk，故 CentralCache 持有的指针绝不被释放。
```
左合并：`left` 保留原起始地址、扩展 pageCount，**chunkMap_ 条目无需改动**；被并入的旧 `chunk` 才删除映射。
右合并：`chunk` 扩展，`right` 的映射删除。
合并完成（pageCount 稳定）后才把最终 Chunk 放入对应空闲桶，避免桶内页数不一致。

### 3.7 锁层次与死锁分析
```
锁序：CentralCache 自旋锁(某大小类) → PageCache::mutex_（单向，从不反向）
```
- ThreadCache：无锁（thread_local）。
- CentralCache：每大小类一把 `atomic_flag` 自旋锁（RAII `SpinLockGuard`，自旋带 yield）。
- PageCache：一把 `std::mutex`。
- PageCache 从不回调 CentralCache，锁序恒为一方 → 无死锁环。

### 3.8 跨平台系统分配
`systemAlloc`：`_WIN32` 走 `VirtualAlloc(MEM_RESERVE|MEM_COMMIT)`，否则走 `mmap(MAP_PRIVATE|MAP_ANONYMOUS)`，均页对齐、失败返回 nullptr；申请到内存后整体 `memset` 清零，首次使用内容确定。

### 3.9 PageCache 批量预取（新增）
**动机**：早期版本向操作系统按需申请——每次分配恰好申请 `numPages` 页。小请求（如 8 页 Chunk）意味着频繁的系统调用（VirtualAlloc/mmap 各一次）与频繁加锁 PageCache 互斥锁，缓存命中前需反复穿越内核。

**做法**：空闲列表无合适 Chunk、必须向操作系统申请时：
```
numPages <  PREFETCH_PAGES(128) : 一次申请 PREFETCH_PAGES 页（512KB）
numPages >= PREFETCH_PAGES(128) : 按需申请 numPages 页（大请求避免过度占用）
```
申请到的一整批先按 `inFreeList = true` 登记（I1），再用 `splitChunkLocked` 切出请求的前 `numPages` 页交付 CentralCache，**剩余页作为空闲 Chunk 挂入空闲列表（I2）**，供后续任意大小类的分配直接复用——多个大小类的首批 Chunk 往往来自同一次系统调用。

**效果/可验证性**：`PageCache::systemAllocCount` 统计系统调用次数。
- `testFirstAllocationPrefetch`：全新进程中第一个分配只触发 1 次系统调用（一次 128 页批量，切 8 页交付、120 页缓存）；
- `testPageCacheBatchPrefetch`：连续 16 次 1 页请求新增系统调用至多 1 次，全部归还（合并）后新增 0 次。

预取剩余的页在归还时与左右邻居合并回原大块，地址空间保持连续，不产生碎片（§3.6 合并逻辑天然覆盖）。

### 3.10 其他工程细节
- 纯 C++11：无结构化绑定、无折叠表达式；MSVC 需 `/std:c++14` 以上、`/utf-8`（源码含中文注释，GBK 误读会吞换行/大括号导致编译诡异失败）。
- `atomic_flag` 默认构造后状态未定义，构造函数中显式 `clear()`。
- `MemoryPool::deallocate` 需与分配时一致的 `size` 才能定位大小类（接口约束，与常见池一致）。
- 内存占用：CentralCache 静态约 1MB（32768 链接表 + 32768 原子锁 + 32768 向量），每线程 ThreadCache 约 512KB（2 × 32768 × 8B）；PageCache 预取顶点缓存一个 128 页（512KB）批次，全部以空闲 Chunk 形式复用。

---

## 4. 文件结构

```
v3/
├── include/
│   ├── Common.h          # 常量、SizeClass、Chunk 结构、SpinLockGuard 声明
│   ├── ThreadCache.h     # 第一层：线程缓存（thread_local 单例，批量申请）
│   ├── CentralCache.h    # 第二层：中心缓存（每大小类自旋锁 + 注册表）
│   ├── PageCache.h       # 第三层：页缓存（互斥锁 + 空闲桶 + 地址映射 + 批量预取）
│   └── MemoryPool.h      # 对外统一入口（转发给 ThreadCache）
├── src/
│   ├── ThreadCache.cpp   # 批量申请/归还、保留阈值策略
│   ├── CentralCache.cpp  # 切块、记账、回收、注册表（核心难点所在）
│   └── PageCache.cpp     # 系统分配、批量预取、切割、左右合并
├── tests/
│   ├── UnitTest.cpp      # 单元测试：基础/写读/多线程/边界/压力/批量预取/批量申请
│   └── PerformanceTest.cpp # 性能对比：内存池 vs new/delete
├── CMakeLists.txt        # CMake 构建（C++11 + ctest，跨平台）
├── build_msvc.bat        # Windows 无 cmake 环境直接用 cl 编译（ASCII-only）
└── README.md
```

---

## 5. 构建与运行

### 5.1 Windows

#### 方式 A：CMake（推荐）
前提：已安装 Visual Studio（含「使用 C++ 的桌面开发」）。在 **x64 Native Tools Command Prompt / 开发者 PowerShell** 中执行（VS 自带的 CMake 已加入 PATH；若不在，可手动把
`<VS安装目录>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin` 加入 PATH）：

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cd build && ctest -C Release --output-on-failure
```

也可直接用 Visual Studio 打开 `CMakeLists.txt`，选择 `unit_test` / `perf_test` 目标运行。

#### 方式 B：直接 cl 编译（无需 CMake）
1. 编辑 `build_msvc.bat` 顶部 `VCVARS` 变量为本机 `vcvars64.bat` 路径
   （或先手动打开 "x64 Native Tools Command Prompt" 再运行本脚本）；
2. 双击或命令行运行：
```bat
build_msvc.bat
cd build_msvc
unit_test.exe     :: 全部通过即正确
perf_test.exe     :: 运行性能对比
```
产出 `build_msvc\unit_test.exe`、`build_msvc\perf_test.exe`。

> 说明：`build_msvc.bat` 必须保持 ASCII-only（cmd.exe 按 OEM 代码页解析批处理，非 ASCII 注释会破坏解析）。
> 编译已通过 MSVC 19.50 验证；`/utf-8` 必须保留（源码为 UTF-8 中文注释）。

#### 方式 C：手动 cl 命令（同方式 B，不依赖脚本）
```bat
call "D:\...\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /EHsc /W4 /O2 /utf-8 /std:c++14 /I include ^
    src\ThreadCache.cpp src\CentralCache.cpp src\PageCache.cpp ^
    tests\UnitTest.cpp /Fe:unit_test.exe
cl /nologo /EHsc /W4 /O2 /utf-8 /std:c++14 /I include ^
    src\ThreadCache.cpp src\CentralCache.cpp src\PageCache.cpp ^
    tests\PerformanceTest.cpp /Fe:perf_test.exe
```

### 5.2 Linux（Ubuntu/Debian 示例，其他发行版同理）

#### 方式 A：CMake（推荐）
```bash
# 安装工具链（如未安装）
sudo apt update && sudo apt install -y build-essential cmake

# 配置（Release）+ 构建 + 运行测试
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
cd build && ctest --output-on-failure
```

#### 方式 B：直接 g++ 编译（无需 CMake）
```bash
g++ -std=c++11 -O2 -pthread -I include -Wall -Wextra \
    src/ThreadCache.cpp src/CentralCache.cpp src/PageCache.cpp \
    tests/UnitTest.cpp -o unit_test
g++ -std=c++11 -O2 -pthread -I include -Wall -Wextra \
    src/ThreadCache.cpp src/CentralCache.cpp src/PageCache.cpp \
    tests/PerformanceTest.cpp -o perf_test

./unit_test   # 正确性：全部通过即 OK
./perf_test   # 性能对比
```

> 说明：`-pthread` 必需（CentralCache 自旋锁用到 `std::this_thread::yield`）。
> 系统分配在 Linux 走 `mmap(MAP_PRIVATE|MAP_ANONYMOUS)`，代码无平台差异（§3.8）。

### 5.3 测试内容
`unit_test` 依次运行：首次分配批量预取 → 基础分配 → 内存写入 → 多线程 → 边界 → 压力 → PageCache 批量预取（缓存复用）→ CentralCache 批量发放。全部通过输出：
`All tests passed successfully!`
`perf_test` 输出内存池 vs `new/delete` 的单线程小对象、多线程、混合大小三组对比。

---

## 6. 修复的问题清单与已知性能情况

### 原实现（v0/v1）重大问题 → 本版本修复
| 问题 | 表现 | 修复方式 |
|------|------|----------|
| 批量获取失效 | 链表遍历中途记账假设失效，块归属错乱 | 地址范围双端缓存校验 + 注册表重查（§3.3） |
| 计数回绕 | 线程缓存计数溢出/漂移，链表与实际不符 | 精确增减、按实际遍历数计数（§3.4） |
| SpanTracker 数组耗尽不记账 | Chunk 块数超过数组容量时空闲计数丢失 | Chunk 内嵌 `freeInCentral` 记账，无数组上限（§3.3） |
| 陈旧条目二次归还 UAF | 已回收 Chunk 的块再次归还，引用已释放内存 | `freeInCentral==totalBlocks` 强不变量 + 注册表摘除 + PageCache `inFreeList` 门禁（§3.5） |
| mmap 不跨平台 | 非 Linux 无法编译/运行 | VirtualAlloc / mmap 双平台实现（§3.8） |
| 命名混乱（span） | 术语歧义 | 统一 Chunk / Block（§2） |

### 本版本新增：全链路批量化
| 行为 | 早期版本 | 本版本 |
|------|----------|--------|
| ThreadCache → CentralCache | 按需/逐次申请，慢速路径频繁进中央锁 | **批量申请**：一次领一整批（`batchSize` 个，约 512B 目标），后续分配无锁命中（§3.4） |
| PageCache → 操作系统 | 按需申请，每次分配一次系统调用 | **批量预取**：一次申请 `PREFETCH_PAGES(128)` 页，切出请求部分，其余缓存复用（§3.9） |

两个行为均有确定性单元测试直接验证（`testFirstAllocationPrefetch`、`testPageCacheBatchPrefetch`、`testCentralCacheBatchFetch`），不依赖时序。

### 性能实测（MSVC 19.50 /O2，Release，单机 x64，机器相关）
| 场景 | 内存池 | new/delete |
|------|--------|-----------|
| 小对象固定大小（8~256B 交替，5 万次） | 2.716 ms | 2.723 ms |
| 多线程（4 线程 × 2.5 万次，混合大小） | 5.867 ms | 2.428 ms |
| 混合大小（8B~4KB，10 万次） | 5.425 ms | 4.210 ms |

单线程已与系统分配器基本持平；**多线程仍较慢**，原因：
1. 快速路径线程本地链表为单链表 FIFO 弹出，nosiness 下命中率低；
2. 中央每大小类只有一把自旋锁，多线程同时慢速路径时互相等待；
3. 多线程压测中的线程退出会丢弃其线程缓存驻留块（thread_local 析构不归还），加剧中央链表空转。

后续优化方向（正确性已稳定）：线程缓存按批锁优化、CentralCache 无锁队列或 sharded 锁、线程退出时归还驻留块、批量/保留阈值自适应调整。