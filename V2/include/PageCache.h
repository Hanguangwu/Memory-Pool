#pragma once

#include "Common.h"

#include <map>
#include <set>
#include <mutex>

namespace Kama_memoryPool
{

// ============================================================
// PageCache（页缓存）：内存池的第三层
// 职责：
//   1. 向操作系统申请大块内存（Chunk），并按页数管理空闲 Chunk；
//   2. 分配时若找到更大的 Chunk 则切割，归还时与左右相邻的空闲 Chunk 合并；
//   3. 维护「起始地址 -> Chunk」映射，供 CentralCache 查询某个
//      Block 归属于哪个 Chunk（用于空闲记账与整块回收）。
// 线程安全：所有公有操作由一把互斥锁保护。
//
// 关键不变量：
//   I1: chunkMap_ 覆盖所有现存 Chunk（含空闲与已分配）。
//   I2: freeChunks_ 恰好包含所有 inFreeList == true 的 Chunk。
//   I3: CentralCache 只引用 inFreeList == false 的 Chunk；
//       PageCache 只合并/删除 inFreeList == true 的 Chunk，
//       因此 CentralCache 持有的 Chunk 指针不会被 PageCache 释放。
// ============================================================
class PageCache
{
public:
    static const size_t PAGE_SIZE = 4096; // 4KB 页大小

    static PageCache& getInstance();

    // 分配指定页数的 Chunk（先查空闲列表，必要时向操作系统申请）
    Chunk* allocateChunk(size_t numPages);

    // 归还 Chunk 到空闲列表，并尝试与左右相邻的空闲 Chunk 合并
    void deallocateChunk(Chunk* chunk);

private:
    PageCache() = default;

    // 查找包含指定地址的 Chunk（调用方必须已持有 mutex_）
    Chunk* findChunkLocked(void* blockAddr) const;

    // 按地址（而非指针值）比较 Chunk，供 std::set 使用
    struct ChunkAddrLess
    {
        bool operator()(const Chunk* a, const Chunk* b) const { return a->addr < b->addr; }
    };

    // 向操作系统申请 numPages 页内存（平台相关：VirtualAlloc / mmap）
    void* systemAlloc(size_t numPages);

private:
    std::map<size_t, std::set<Chunk*, ChunkAddrLess>> freeChunks_; // 页数 -> 空闲 Chunk 集合
    std::map<void*, Chunk*> chunkMap_;                             // 起始地址 -> Chunk
    mutable std::mutex mutex_;
};

} // namespace Kama_memoryPool