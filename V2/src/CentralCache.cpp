#include "../include/CentralCache.h"
#include "../include/PageCache.h"

#include <thread>
#include <vector>

namespace Kama_memoryPool
{

// ------------------------------------------------------------
// SpinLockGuard 实现：自旋等待（带让步）直到拿到锁，析构释放。
// ------------------------------------------------------------
SpinLockGuard::SpinLockGuard(std::atomic_flag& flag)
    : flag_(flag)
{
    while (flag_.test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield(); // 避免忙等待过度消耗 CPU
    }
}

SpinLockGuard::~SpinLockGuard()
{
    flag_.clear(std::memory_order_release);
}

// ------------------------------------------------------------
// 构造函数：空闲链表全部置空；自旋锁逐个清零。
// 注意：atomic_flag 默认构造后状态未定义，必须显式 clear。
// ------------------------------------------------------------
CentralCache::CentralCache()
{
    freeList_.fill(nullptr);
    for (size_t i = 0; i < FREE_LIST_SIZE; ++i)
    {
        locks_[i].clear();
    }
}

CentralCache& CentralCache::getInstance()
{
    static CentralCache instance;
    return instance;
}

// ------------------------------------------------------------
// 向 PageCache 申请 Chunk 并切成 Block 链表，挂到 index 类的链表头。
// 返回该链表头（始终非空：totalBlocks 至少为 1）。
// 调用前提：调用方已持有 locks_[index]。
// ------------------------------------------------------------
void* CentralCache::createChunkFor(size_t index)
{
    const size_t blockSize = indexToBlockSize(index);

    // 小对象统一申请 8 页（32KB）的 Chunk，大对象按实际大小向上取整
    const size_t chunkPages =
        (blockSize <= CHUNK_PAGES * PageCache::PAGE_SIZE)
            ? CHUNK_PAGES
            : (blockSize + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    Chunk* chunk = PageCache::getInstance().allocateChunk(chunkPages);
    if (chunk == nullptr) return nullptr;

    // 初始化切割信息
    chunk->blockSize     = blockSize;
    chunk->totalBlocks   = (chunk->pageCount * PageCache::PAGE_SIZE) / blockSize; // >= 1
    chunk->freeInCentral = chunk->totalBlocks; // 初始全部位于中央链表

    // 把整个 Chunk 串成 Block 链表：内嵌 next 指针存放于每个 Block 的前 8 字节
    char* base = static_cast<char*>(chunk->addr);
    for (size_t i = 0; i + 1 < chunk->totalBlocks; ++i)
    {
        *reinterpret_cast<void**>(base + i * blockSize) = base + (i + 1) * blockSize;
    }
    *reinterpret_cast<void**>(base + (chunk->totalBlocks - 1) * blockSize) = nullptr;

    // 登记为当前大小类的活跃 Chunk（供块归属查询）
    ChunkSlot slot;
    slot.start = reinterpret_cast<uintptr_t>(chunk->addr);
    slot.end   = slot.start + chunk->pageCount * PageCache::PAGE_SIZE;
    slot.chunk = chunk;
    chunkRegistry_[index].push_back(slot);

    freeList_[index] = chunk->addr;
    return chunk->addr;
}

// ------------------------------------------------------------
// 在 index 类的活跃 Chunk 注册表中查找包含指定地址的 Chunk。
// 每类活跃 Chunk 通常只有 1~3 个，线性扫描即可。
// 调用前提：调用方已持有 locks_[index]。
// ------------------------------------------------------------
Chunk* CentralCache::findChunkInRegistry(size_t index, void* blockAddr) const
{
    const uintptr_t addr = reinterpret_cast<uintptr_t>(blockAddr);
    const std::vector<ChunkSlot>& slots = chunkRegistry_[index];
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (addr >= slots[i].start && addr < slots[i].end)
        {
            return slots[i].chunk;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------
// 整块回收时从注册表摘除指定 Chunk。
// 调用前提：调用方已持有 locks_[index]。
// ------------------------------------------------------------
void CentralCache::eraseChunkFromRegistry(size_t index, Chunk* chunk)
{
    std::vector<ChunkSlot>& slots = chunkRegistry_[index];
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].chunk == chunk)
        {
            // 与末尾元素交换后弹出，保持 O(1)
            slots[i] = slots.back();
            slots.pop_back();
            return;
        }
    }
}

// ------------------------------------------------------------
// 从中央链表取出至多 batchNum 个 Block。
// 返回批次链表头（以 nullptr 结尾），剩余块留在中央链表。
// 每取走一个块，对应 Chunk 的 freeInCentral 减一。
// ------------------------------------------------------------
void* CentralCache::fetchBlocks(size_t index, size_t batchNum)
{
    SpinLockGuard guard(locks_[index]);

    // 链表为空则先向 PageCache 申请 Chunk 并切割
    if (freeList_[index] == nullptr)
    {
        if (createChunkFor(index) == nullptr) return nullptr;
    }

    void* batchHead = freeList_[index];
    void* cursor    = batchHead;
    void* prev      = nullptr;
    size_t got      = 0;

    // 记账缓存：链表按 next 指针连接，遍历顺序不代表地址连续
    // （归还的块可能来自任意 Chunk、地址任意交错），因此只有当下
    // 一个块的地址确实落在当前 Chunk 的地址范围内时才能复用缓存，
    // 否则必须重新查询，保证 freeInCentral 记账准确。
    Chunk*    curChunk   = nullptr;
    uintptr_t chunkStart = 0;
    uintptr_t chunkEnd   = 0;

    while (cursor != nullptr && got < batchNum)
    {
        const uintptr_t blockAddr = reinterpret_cast<uintptr_t>(cursor);

        if (curChunk == nullptr ||
            blockAddr < chunkStart || blockAddr >= chunkEnd)
        {
            curChunk = findChunkInRegistry(index, cursor);
            if (curChunk != nullptr)
            {
                chunkStart = reinterpret_cast<uintptr_t>(curChunk->addr);
                chunkEnd   = chunkStart + curChunk->pageCount * PageCache::PAGE_SIZE;
            }
            else
            {
                chunkStart = 0;
                chunkEnd   = 0;
            }
        }

        if (curChunk != nullptr)
        {
            --curChunk->freeInCentral; // 该 Block 离开中央链表
        }

        prev   = cursor;
        cursor = *reinterpret_cast<void**>(cursor);
        ++got;
    }

    if (got == 0) return nullptr; // 防御：创建 Chunk 后链表必非空

    // 断开批次与剩余链表
    *reinterpret_cast<void**>(prev) = nullptr;
    freeList_[index] = cursor;

    return batchHead;
}

// ------------------------------------------------------------
// 归还一条含 blockCount 个 Block 的链表（head 起，内嵌 next 已串好）。
// 流程：
//   1. 计账：每块所属 Chunk 的 freeInCentral 加一；
//      若达到 totalBlocks 说明整块空闲，记入待回收列表；
//   2. 拼接：把归还链表接到中央链表头部；
//   3. 回收：把整块空闲的 Chunk 的所有 Block 从链表摘除，归还 PageCache。
// ------------------------------------------------------------
void CentralCache::returnBlocks(void* head, size_t blockCount, size_t index)
{
    if (head == nullptr || blockCount == 0) return;

    SpinLockGuard guard(locks_[index]);

    // ---- 1. 计账（拼接之前，归还链表仍独立且以 nullptr 结尾）----
    std::vector<Chunk*> reclaimList;

    void*  cursor    = head;
    size_t actual    = 0;
    Chunk* curChunk  = nullptr;
    uintptr_t chunkStart = 0;
    uintptr_t chunkEnd   = 0;

    while (cursor != nullptr && actual < blockCount)
    {
        void* nextBlock = *reinterpret_cast<void**>(cursor);
        const uintptr_t blockAddr = reinterpret_cast<uintptr_t>(cursor);

        // 与 fetchBlocks 相同的记账缓存规则：地址不在当前 Chunk 范围内
        // 时必须重新查询（链表遍历顺序不代表地址连续）。
        if (curChunk == nullptr ||
            blockAddr < chunkStart || blockAddr >= chunkEnd)
        {
            curChunk = findChunkInRegistry(index, cursor);
            if (curChunk != nullptr)
            {
                chunkStart = reinterpret_cast<uintptr_t>(curChunk->addr);
                chunkEnd   = chunkStart + curChunk->pageCount * PageCache::PAGE_SIZE;
            }
            else
            {
                chunkStart = 0;
                chunkEnd   = 0;
            }
        }

        if (curChunk != nullptr)
        {
            ++curChunk->freeInCentral; // 该 Block 回到中央链表
            if (curChunk->freeInCentral == curChunk->totalBlocks)
            {
                // 整块空闲：所有 Block 都已在中央链表中，可安全回收
                reclaimList.push_back(curChunk);
            }
        }

        cursor = nextBlock;
        ++actual;
    }
    // 防御：若实际块数与调用方计数不符（正常不会发生），以实际为准
    blockCount = actual;

    // ---- 2. 拼接：找到归还链表的尾，接入中央链表头部 ----
    void* tail = head;
    for (size_t i = 1; i < blockCount; ++i)
    {
        tail = *reinterpret_cast<void**>(tail);
    }
    *reinterpret_cast<void**>(tail) = freeList_[index];
    freeList_[index] = head;

    // ---- 3. 回收：摘除整块空闲 Chunk 的全部 Block 并归还 PageCache ----
    for (size_t r = 0; r < reclaimList.size(); ++r)
    {
        Chunk* chunk = reclaimList[r];

        const uintptr_t start = reinterpret_cast<uintptr_t>(chunk->addr);
        const uintptr_t end   = start + chunk->pageCount * PageCache::PAGE_SIZE;

        void* newHead  = nullptr;
        void* keptTail = nullptr;
        void* cur      = freeList_[index];

        while (cur != nullptr)
        {
            void* next = *reinterpret_cast<void**>(cur);
            const uintptr_t addr = reinterpret_cast<uintptr_t>(cur);

            if (addr < start || addr >= end)
            {
                // 保留：按原顺序重连
                if (keptTail == nullptr) newHead = cur;
                else *reinterpret_cast<void**>(keptTail) = cur;
                keptTail = cur;
            }
            // 位于 [start, end) 内的 Block 直接摘除
            cur = next;
        }
        if (keptTail != nullptr)
        {
            *reinterpret_cast<void**>(keptTail) = nullptr;
        }
        freeList_[index] = newHead;

        // 从活跃注册表摘除后归还 PageCache（此后该 Chunk 不再属于本大小类）
        eraseChunkFromRegistry(index, chunk);
        PageCache::getInstance().deallocateChunk(chunk);
    }
}

} // namespace Kama_memoryPool