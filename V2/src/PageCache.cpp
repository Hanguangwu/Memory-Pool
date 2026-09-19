#include "../include/PageCache.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace Kama_memoryPool
{

PageCache& PageCache::getInstance()
{
    static PageCache instance;
    return instance;
}

// ------------------------------------------------------------
// 向操作系统申请 numPages 页连续内存（页对齐），并清零。
// Windows 用 VirtualAlloc，POSIX 用 mmap，保证跨平台可编译运行。
// ------------------------------------------------------------
void* PageCache::systemAlloc(size_t numPages)
{
    const size_t size = numPages * PAGE_SIZE;

#ifdef _WIN32
    void* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (ptr == nullptr) return nullptr;
#else
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;
#endif

    std::memset(ptr, 0, size); // 清零，保证首次使用得到确定性内容
    return ptr;
}

// ------------------------------------------------------------
// 分配指定页数的 Chunk：
//   1. 优先取空闲列表中不小于所需页数的最小 Chunk（lower_bound）；
//   2. 若其偏大则切割，剩余部分作为新的空闲 Chunk；
//   3. 都没有则向操作系统申请。
// ------------------------------------------------------------
Chunk* PageCache::allocateChunk(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = freeChunks_.lower_bound(numPages);
    if (it != freeChunks_.end())
    {
        // 摘除候选 Chunk
        Chunk* chunk = *it->second.begin();
        it->second.erase(it->second.begin());
        if (it->second.empty())
        {
            freeChunks_.erase(it);
        }

        // 切割：剩余部分回收到空闲列表
        if (chunk->pageCount > numPages)
        {
            Chunk* rest = new Chunk;
            rest->addr       = static_cast<char*>(chunk->addr) + numPages * PAGE_SIZE;
            rest->pageCount  = chunk->pageCount - numPages;
            rest->inFreeList = true;

            chunkMap_[rest->addr] = rest;          // 不变量 I1
            freeChunks_[rest->pageCount].insert(rest); // 不变量 I2

            chunk->pageCount = numPages;
        }

        chunk->inFreeList = false;
        chunkMap_[chunk->addr] = chunk;
        return chunk;
    }

    // 空闲列表无合适 Chunk，向操作系统申请
    void* memory = systemAlloc(numPages);
    if (memory == nullptr) return nullptr;

    Chunk* chunk = new Chunk;
    chunk->addr       = memory;
    chunk->pageCount  = numPages;
    chunk->inFreeList = false;

    chunkMap_[chunk->addr] = chunk;
    return chunk;
}

// ------------------------------------------------------------
// 归还 Chunk：先并入空闲列表，再循环尝试与左右相邻的
// 空闲 Chunk 合并（相邻合并减少碎片、利于大请求复用）。
// 合并会改变 pageCount，因此合并完成后才把 chunk 放入对应桶。
// ------------------------------------------------------------
void PageCache::deallocateChunk(Chunk* chunk)
{
    std::lock_guard<std::mutex> lock(mutex_);
    chunk->inFreeList = true;

    bool merged = true;
    while (merged)
    {
        merged = false;

        // 左邻居：包含 chunk 前一地址、且末尾恰好与 chunk 相接
        const uintptr_t chunkStart = reinterpret_cast<uintptr_t>(chunk->addr);
        Chunk* left = findChunkLocked(static_cast<char*>(chunk->addr) - 1);
        if (left != nullptr && left->inFreeList &&
            reinterpret_cast<uintptr_t>(left->addr) + left->pageCount * PAGE_SIZE == chunkStart)
        {
            // 从空闲桶摘除 left（页数即将变化）
            freeChunks_[left->pageCount].erase(left);
            if (freeChunks_[left->pageCount].empty())
            {
                freeChunks_.erase(left->pageCount);
            }

            // left 保留原有起始地址、仅扩大范围，chunkMap_ 条目无需改动（不变量 I1）
            left->pageCount += chunk->pageCount;

            // 删除旧 chunk 对象及其映射
            chunkMap_.erase(chunk->addr);
            delete chunk;
            chunk = left;
            merged = true;
            continue;
        }

        // 右邻居：起始地址恰好等于 chunk 的末尾
        const uintptr_t chunkEnd = chunkStart + chunk->pageCount * PAGE_SIZE;
        Chunk* right = findChunkLocked(reinterpret_cast<void*>(chunkEnd));
        if (right != nullptr && right->inFreeList &&
            reinterpret_cast<uintptr_t>(right->addr) == chunkEnd)
        {
            freeChunks_[right->pageCount].erase(right);
            if (freeChunks_[right->pageCount].empty())
            {
                freeChunks_.erase(right->pageCount);
            }
            chunkMap_.erase(right->addr);

            chunk->pageCount += right->pageCount;
            delete right;
            chunk->inFreeList = true;
            merged = true;
            continue;
        }
    }

    // 合并完成，放入空闲列表
    freeChunks_[chunk->pageCount].insert(chunk);
}

// ------------------------------------------------------------
// 查找包含指定地址的 Chunk（调用方必须已持有 mutex_）。
// 借助「地址 -> Chunk」映射的 upper_bound 实现 O(log n) 查询。
// 用于 deallocateChunk 的左右邻居判定，以及 findChunk 的委托。
// ------------------------------------------------------------
Chunk* PageCache::findChunkLocked(void* blockAddr) const
{
    auto it = chunkMap_.upper_bound(blockAddr);
    if (it == chunkMap_.begin()) return nullptr;
    --it;

    Chunk* chunk = it->second;
    const uintptr_t start = reinterpret_cast<uintptr_t>(chunk->addr);
    if (reinterpret_cast<uintptr_t>(blockAddr) < start + chunk->pageCount * PAGE_SIZE)
    {
        return chunk;
    }
    return nullptr;
}

} // namespace Kama_memoryPool