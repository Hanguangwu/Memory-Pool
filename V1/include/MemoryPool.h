#pragma once

// ============================================================================
// MemoryPool.h
// 基于哈希映射的多种定长内存分配器（内存池）
//
// 术语约定：
//   Chunk : 大块内存，一次向系统申请的一大段连续内存（如 4096 字节）
//   Block : 小块内存，池内定长切分的最小分配单元，直接交给使用者
//
// 结构：
//   HashBucket : 哈希桶，按请求字节大小把分配请求映射到 64 个定长内存池
//   MemoryPool : 单个定长内存池，管理若干 Chunk 与其中的 Block
//   newElement / deleteElement : 类型安全的对象级接口
// ============================================================================

#include <cstddef>   // std::size_t
#include <cstdint>   // std::uintptr_t
#include <mutex>     // std::mutex, std::lock_guard
#include <new>       // ::operator new / ::operator delete
#include <utility>   // std::forward

namespace KamaMemoryPool
{

// ---------------------------------------------------------------------------
// 可调参数（均为 2 的幂倍数，保证各池 Block 无缝紧密排列）
// ---------------------------------------------------------------------------
constexpr std::size_t kBaseBlockSize    = 8;     // 最小 Block 大小（字节）
constexpr std::size_t kLargestBlockSize = 512;   // 池内最大 Block 大小（字节）
constexpr std::size_t kPoolCount        = kLargestBlockSize / kBaseBlockSize; // 64 个池
constexpr std::size_t kDefaultChunkSize = 4096;  // 每个 Chunk 从系统申请的大小（字节）

constexpr std::size_t kPoolIndexOf(std::size_t size)
{
    // 把请求大小映射为池下标：(size + 7) / 8 - 1
    return (size + kBaseBlockSize - 1) / kBaseBlockSize - 1;
}

// ---------------------------------------------------------------------------
// MemoryPool：单个定长内存池
//
// 一个池只服务一种 Block 大小。池内维护：
//   - Chunk 链表       ：记录所有从系统申请来的 Chunk，用于析构时统一回收
//   - 空闲 Block 链表  ：已被释放、可复用的 Block（先进后出栈）
//   - 未用 Block 游标  ：当前 Chunk 中尚未划出的连续 Block 区
//
// 线程安全：整个池共用一把互斥锁；不同大小的池各自持锁，互不干扰。
// ---------------------------------------------------------------------------
class MemoryPool
{
public:
    explicit MemoryPool(std::size_t chunkSize = kDefaultChunkSize);
    ~MemoryPool();

    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // 设置本池的 Block 大小。必须在首次分配之前调用一次。
    void init(std::size_t blockSize);

    // 分配一个 Block；池未初始化时返回 nullptr。
    void* allocate();

    // 归还一个 Block；ptr 必须来自本池的 allocate()。
    void deallocate(void* ptr);

    std::size_t blockSize() const { return blockSize_; }

private:
    // 每个 Block 在空闲期间，其头部存放下一个空闲 Block 的指针。
    // 各池 Block 大小 >= 8 字节，足够容纳指针（32/64 位均满足）。
    struct Block
    {
        Block* next; // 仅在 Block 位于空闲链表时有效
    };

    // 每个 Chunk 的头部存放上一个 Chunk 的指针，串成链表供析构遍历。
    struct Chunk
    {
        Chunk* prev;
    };

    // 向系统申请一个新 Chunk，接在 Chunk 链表头部，并重置未用区游标。
    // 前置条件：调用方已持有 poolMutex_。
    void allocateNewChunk();

    // 返回把地址 address 向上对齐到 alignment 所需填充的字节数。
    static std::size_t padPointer(std::uintptr_t address, std::size_t alignment);

private:
    const std::size_t chunkSize_;  // 每个 Chunk 的大小
    std::size_t       blockSize_;  // 本池 Block 的大小（0 表示未初始化）

    Chunk*  firstChunk_;      // Chunk 链表头（用于析构回收）
    Block*  freeListHead_;    // 空闲 Block 链表头
    Block*  nextUnusedBlock_; // 当前 Chunk 中下一个未使用的 Block
    Block*  chunkEnd_;        // 当前 Chunk 的结尾（未用区不得越过此边界）

    std::mutex  poolMutex_;   // 保护本池全部状态
};

// ---------------------------------------------------------------------------
// HashBucket：哈希桶，把任意请求大小路由到合适的定长内存池
//
// 请求大小 <= 512 字节 → 按 8 字节向上取整对齐到池下标，从对应池分配
// 请求大小 >  512 字节 → 直接交给系统分配器（operator new / delete）
// ---------------------------------------------------------------------------
class HashBucket
{
public:
    // 一次性初始化全部 64 个池，线程安全且可重复调用（内部 call_once）。
    static void initialize();

    static void* allocate(std::size_t size);
    static void deallocate(void* ptr, std::size_t size);

private:
    static MemoryPool& poolFor(std::size_t index);
};

// ---------------------------------------------------------------------------
// 对象级接口：分配内存 + 定位构造 / 先析构 + 归还内存
// ---------------------------------------------------------------------------
template <typename T, typename... Args>
T* newElement(Args&&... args)
{
    void* memory = HashBucket::allocate(sizeof(T));
    if (memory == nullptr)
        return nullptr;

    // 定位构造；若构造函数抛出异常，归还内存后继续上抛
    try
    {
        return new (memory) T(std::forward<Args>(args)...);
    }
    catch (...)
    {
        HashBucket::deallocate(memory, sizeof(T));
        throw;
    }
}

template <typename T>
void deleteElement(T* ptr)
{
    if (ptr == nullptr)
        return;
    ptr->~T();
    HashBucket::deallocate(reinterpret_cast<void*>(ptr), sizeof(T));
}

} // namespace KamaMemoryPool