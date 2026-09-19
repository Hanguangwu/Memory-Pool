#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <atomic>

namespace Kama_memoryPool
{

// ============================================================
// 基础配置常量
// ============================================================
constexpr size_t ALIGNMENT       = sizeof(void*);      // 对齐粒度：x64 下为 8 字节
constexpr size_t MAX_BYTES       = 256 * 1024;         // 大小类上限：256KB，超过直接走 malloc/free
constexpr size_t FREE_LIST_SIZE  = MAX_BYTES / ALIGNMENT; // 大小类个数：32768
constexpr size_t CHUNK_PAGES     = 8;                  // 小对象 Chunk 的默认页数（8 * 4KB = 32KB）

// 由大小类索引得到该类的块大小（8, 16, 24, ...）
inline size_t indexToBlockSize(size_t index)
{
    return (index + 1) * ALIGNMENT;
}

// ============================================================
// 大小类：请求字节数 -> 大小类索引
// 说明：getIndex(x) == getIndex(roundUp(x))，因此分配/释放
// 时无需先做向上取整，直接传入原始请求字节数即可。
// ============================================================
class SizeClass
{
public:
    static size_t getIndex(size_t bytes)
    {
        bytes = std::max(bytes, ALIGNMENT);   // 至少占一个对齐单元（0 请求归入 8 字节类）
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};

// ============================================================
// Chunk：PageCache 从操作系统获取的大块内存（多页连续），
// 由 CentralCache 按块大小切成若干个 Block。
// 命名约定：大块内存区统一称 Chunk，小块统一称 Block。
// ============================================================
struct Chunk
{
    void*  addr;          // 起始地址（页对齐）
    size_t pageCount;     // 页数
    size_t blockSize;     // 本 Chunk 当前切割的块大小（由 CentralCache 在切块时写入）
    size_t totalBlocks;   // 切割出的块总数
    size_t freeInCentral; // 当前位于中央缓存空闲链表中的块数；
                          // 只在 CentralCache 持有所属大小类自旋锁期间访问，无需原子
    bool   inFreeList;    // 是否位于 PageCache 的空闲列表中；
                          // 为 true 时 CentralCache 不得再引用它（PageCache 可能合并或删除）
};

// ============================================================
// 自旋锁 RAII 守卫：中央缓存每个大小类一把自旋锁
// ============================================================
class SpinLockGuard
{
public:
    explicit SpinLockGuard(std::atomic_flag& flag);
    ~SpinLockGuard();

    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    std::atomic_flag& flag_;
};

} // namespace Kama_memoryPool