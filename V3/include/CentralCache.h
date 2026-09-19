#pragma once

#include "Common.h"

#include <array>
#include <atomic>
#include <vector>

namespace Kama_memoryPool
{

// ============================================================
// CentralCache（中心缓存）：内存池的第二层
// 职责：
//   1. 管理所有线程共享的空闲 Block 链表（按大小类划分）；
//   2. 向 ThreadCache 批量发放 Block、批量接收归还；
//   3. 链表耗尽时向 PageCache 申请 Chunk 并切成 Block；
//   4. 某个 Chunk 的全部 Block 都回到中央链表时，整块归还给 PageCache。
// 线程安全：每个大小类一把自旋锁，全部链表操作在锁内完成。
//
// 记账不变量：chunk->freeInCentral 恒等于该 Chunk 当前位于
// 中央空闲链表中的块数。取块时递减、归还时递增；当它等于
// totalBlocks 时说明整块空闲，可以安全地整块回收。
//
// 性能要点：为确定每个块归属哪个 Chunk，使用每大小类内部的
// 「活跃 Chunk 注册表」（每个大小类通常只有 1~3 个活跃 Chunk，
// 线性扫描即可），全程只在自旋锁内完成，不在分配热路径上
// 访问 PageCache 的全局互斥锁。
// ============================================================
class CentralCache
{
public:
    static CentralCache& getInstance();

    // 从中央链表取至多 batchNum 个 Block，返回链表头（以 nullptr 结尾）
    void* fetchBlocks(size_t index, size_t batchNum);

    // 归还一条含 blockCount 个 Block 的链表（内嵌 next 指针已串好、尾部为 nullptr）
    void returnBlocks(void* head, size_t blockCount, size_t index);

private:
    CentralCache(); // 初始化空闲链表与自旋锁

    // 向 PageCache 申请 Chunk 并切成 Block 链表，挂到 index 类的链表头
    void* createChunkFor(size_t index);

    // 在 index 类的活跃 Chunk 注册表中查找包含指定地址的 Chunk
    Chunk* findChunkInRegistry(size_t index, void* blockAddr) const;

    // 从注册表摘除指定 Chunk（整块回收时调用）
    void eraseChunkFromRegistry(size_t index, Chunk* chunk);

private:
    // 活跃 Chunk 注册表条目
    struct ChunkSlot
    {
        uintptr_t start; // Chunk 起始地址（整数化，便于区间比较）
        uintptr_t end;   // 起始地址 + pageCount * PAGE_SIZE
        Chunk*    chunk; // Chunk 对象指针
    };

    std::array<void*, FREE_LIST_SIZE>                 freeList_;     // 每个大小类的空闲链表头（受对应自旋锁保护）
    std::array<std::atomic_flag, FREE_LIST_SIZE>      locks_;        // 每个大小类的自旋锁
    std::array<std::vector<ChunkSlot>, FREE_LIST_SIZE> chunkRegistry_; // 每个大小类的活跃 Chunk 列表
};

} // namespace Kama_memoryPool