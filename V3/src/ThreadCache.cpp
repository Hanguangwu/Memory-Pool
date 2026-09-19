#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"

#include <cstdlib>

namespace Kama_memoryPool
{

ThreadCache::ThreadCache()
{
    freeList_.fill(nullptr);
    freeCount_.fill(0);
}

ThreadCache* ThreadCache::getInstance()
{
    static thread_local ThreadCache instance;
    return &instance;
}

// ------------------------------------------------------------
// 分配 size 字节：
//   - 0 请求按一个对齐单元处理；
//   - 超过 MAX_BYTES 直接走系统分配器；
//   - 否则优先从线程本地链表弹出，空则向中央缓存批量领取。
// ------------------------------------------------------------
void* ThreadCache::allocate(size_t size)
{
    if (size == 0)
    {
        size = ALIGNMENT;
    }

    if (size > MAX_BYTES)
    {
        return std::malloc(size);
    }

    const size_t index = SizeClass::getIndex(size);

    // 快速路径：线程本地链表有货
    if (void* block = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(block); // 弹出头块
        --freeCount_[index];
        return block;
    }

    // 慢速路径：从中央缓存批量领取
    return fetchFromCentral(index);
}

// ------------------------------------------------------------
// 释放 size 字节的内存块：
//   - 超过 MAX_BYTES 的对象走系统释放；
//   - 否则压入线程本地链表，超过保留阈值时归还多余部分。
// 注意：deallocate 需要与分配时一致的 size 才能定位正确的大小类。
// ------------------------------------------------------------
void ThreadCache::deallocate(void* ptr, size_t size)
{
    if (ptr == nullptr) return;

    if (size > MAX_BYTES)
    {
        std::free(ptr);
        return;
    }

    const size_t index = SizeClass::getIndex(size);

    // 压入线程本地链表（内嵌 next 指针）
    *reinterpret_cast<void**>(ptr) = freeList_[index];
    freeList_[index] = ptr;
    ++freeCount_[index];

    // 超过阈值则把尾部多余块归还中央缓存
    if (freeCount_[index] > keepLimit(index))
    {
        returnToCentral(index);
    }
}

// ------------------------------------------------------------
// 每批领取的块数：目标约 512 字节，取值范围 2..64。
// 小块多取以摊薄锁开销，大块少取避免驻留过多内存。
// ------------------------------------------------------------
size_t ThreadCache::batchSize(size_t index)
{
    const size_t blockSize = indexToBlockSize(index);
    size_t batch = 512 / blockSize;
    if (batch < 2)  batch = 2;
    if (batch > 64) batch = 64;
    return batch;
}

// ------------------------------------------------------------
// 线程内每个大小类最多保留的块数：约两批，避免频繁向中央缓存来回。
// ------------------------------------------------------------
size_t ThreadCache::keepLimit(size_t index)
{
    // 线程内最多保留约两批，避免频繁向中央缓存来回
    return std::max(batchSize(index) * 2, size_t(32));
}

// ------------------------------------------------------------
// 从中央缓存领取一批 Block（batchSize 个，而非按需 1 个）：
// 第一个返回给调用者，其余挂入线程本地链表，并同步维护链表计数。
// 这样慢速路径只进一次 CentralCache 的锁，摊薄锁竞争与记账开销。
// ------------------------------------------------------------
void* ThreadCache::fetchFromCentral(size_t index)
{
    void* head = CentralCache::getInstance().fetchBlocks(index, batchSize(index));
    if (head == nullptr) return nullptr;

    // 统计本批实际块数（防御式自行计数，不依赖中央缓存返回值）
    size_t n = 0;
    for (void* cursor = head; cursor != nullptr;
         cursor = *reinterpret_cast<void**>(cursor))
    {
        ++n;
    }

    // 返回第一个块，其余挂入线程本地链表
    freeList_[index] = *reinterpret_cast<void**>(head);
    freeCount_[index] += (n - 1);
    return head;
}

// ------------------------------------------------------------
// 把链表尾部（最近最少被复用的块）超出保留阈值的部分归还中央缓存。
// 保留段在前、归还段（尾部）在后，保持块的内存局部性。
// ------------------------------------------------------------
void ThreadCache::returnToCentral(size_t index)
{
    const size_t keep        = keepLimit(index);
    const size_t count       = freeCount_[index];
    if (count <= keep) return;

    const size_t returnCount = count - keep;

    // 从头沿 next 走 keep 步，定位「保留段末尾」
    void* cursor = freeList_[index];
    for (size_t i = 1; i < keep && cursor != nullptr; ++i)
    {
        cursor = *reinterpret_cast<void**>(cursor);
    }

    // 防御：链表长度与计数不一致（正常流程不会发生），重置状态
    if (cursor == nullptr)
    {
        freeList_[index]  = nullptr;
        freeCount_[index] = 0;
        return;
    }

    // 断开：保留段在前，归还段（尾部）在后
    void* tail = *reinterpret_cast<void**>(cursor);
    *reinterpret_cast<void**>(cursor) = nullptr;
    freeCount_[index] = keep;

    CentralCache::getInstance().returnBlocks(tail, returnCount, index);
}

} // namespace Kama_memoryPool