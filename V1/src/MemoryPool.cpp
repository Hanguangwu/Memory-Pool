#include "MemoryPool.h"

#include <cassert>

namespace KamaMemoryPool
{

// ============================================================================
// MemoryPool 实现
// ============================================================================

MemoryPool::MemoryPool(std::size_t chunkSize)
    : chunkSize_(chunkSize)
    , blockSize_(0)
    , firstChunk_(nullptr)
    , freeListHead_(nullptr)
    , nextUnusedBlock_(nullptr)
    , chunkEnd_(nullptr)
{
    // chunkSize_ 必须足以容纳 Chunk 头 + 至少一个 Block
    assert(chunkSize_ > sizeof(Chunk) + sizeof(Block));
}

MemoryPool::~MemoryPool()
{
    // 沿 Chunk 链表逐个归还给系统
    Chunk* chunk = firstChunk_;
    while (chunk != nullptr)
    {
        Chunk* prev = chunk->prev;
        ::operator delete(chunk);
        chunk = prev;
    }
}

void MemoryPool::init(std::size_t blockSize)
{
    assert(blockSize > 0);
    assert(blockSize % kBaseBlockSize == 0);       // 必须是 8 的倍数
    assert(blockSize <= kLargestBlockSize);
    assert(chunkSize_ > sizeof(Chunk) + blockSize); // 一个 Chunk 至少容得下一个 Block

    blockSize_ = blockSize;
    firstChunk_      = nullptr;
    freeListHead_    = nullptr;
    nextUnusedBlock_ = nullptr;
    chunkEnd_        = nullptr;
}

void* MemoryPool::allocate()
{
    // 池未初始化时拒绝分配
    if (blockSize_ == 0)
        return nullptr;

    std::lock_guard<std::mutex> guard(poolMutex_);

    // 1. 优先复用空闲链表中的 Block（栈顶出栈）
    if (freeListHead_ != nullptr)
    {
        Block* reused = freeListHead_;
        freeListHead_ = reused->next;
        return reused;
    }

    // 2. 尚无 Chunk，或当前 Chunk 余量不足一个 Block 时，申请新的 Chunk
    const bool needNewChunk =
        nextUnusedBlock_ == nullptr ||
        (reinterpret_cast<char*>(chunkEnd_) - reinterpret_cast<char*>(nextUnusedBlock_)) <
            static_cast<std::ptrdiff_t>(blockSize_);
    if (needNewChunk)
        allocateNewChunk();

    // 3. 从当前 Chunk 的未用区划出一块
    Block* fresh = nextUnusedBlock_;
    nextUnusedBlock_ =
        reinterpret_cast<Block*>(reinterpret_cast<char*>(nextUnusedBlock_) + blockSize_);
    return fresh;
}

void MemoryPool::deallocate(void* ptr)
{
    if (ptr == nullptr)
        return;

    std::lock_guard<std::mutex> guard(poolMutex_);

    // 栈顶入栈：复用该 Block 头部存放的 next 指针
    Block* block = reinterpret_cast<Block*>(ptr);
    block->next = freeListHead_;
    freeListHead_ = block;
}

void MemoryPool::allocateNewChunk()
{
    // 一次向系统申请整块连续内存，头部存放 Chunk 链表指针
    Chunk* chunk = reinterpret_cast<Chunk*>(::operator new(chunkSize_));
    chunk->prev = firstChunk_;
    firstChunk_ = chunk;

    // 负载区起点：Chunk 头之后，按 blockSize_ 对齐。
    // 由于各池 Block 大小均为 8 的倍数（2 的幂），对齐后连续划块不会留缝。
    std::uintptr_t payloadAddress =
        reinterpret_cast<std::uintptr_t>(chunk) + sizeof(Chunk);
    payloadAddress += padPointer(payloadAddress, blockSize_);

    nextUnusedBlock_ = reinterpret_cast<Block*>(payloadAddress);
    chunkEnd_        = reinterpret_cast<Block*>(
        reinterpret_cast<std::uintptr_t>(chunk) + chunkSize_);
}

std::size_t MemoryPool::padPointer(std::uintptr_t address, std::size_t alignment)
{
    const std::size_t remainder = address % alignment;
    return remainder == 0 ? 0 : alignment - remainder;
}

// ============================================================================
// HashBucket 实现
// ============================================================================

// 单例池数组：函数局部静态变量，C++11 起保证线程安全的懒初始化
MemoryPool& HashBucket::poolFor(std::size_t index)
{
    static MemoryPool pools_[kPoolCount];
    return pools_[index];
}

void HashBucket::initialize()
{
    static std::once_flag flag;
    std::call_once(flag, []()
    {
        for (std::size_t i = 0; i < kPoolCount; ++i)
        {
            // 第 i 个池的 Block 大小 = (i + 1) * 8：8, 16, ..., 512
            poolFor(i).init((i + 1) * kBaseBlockSize);
        }
    });
}

void* HashBucket::allocate(std::size_t size)
{
    if (size == 0)
        return nullptr;

    // 超过池内最大 Block 大小的请求直接交给系统分配器
    if (size > kLargestBlockSize)
        return ::operator new(size);

    return poolFor(kPoolIndexOf(size)).allocate();
}

void HashBucket::deallocate(void* ptr, std::size_t size)
{
    if (ptr == nullptr)
        return;

    if (size > kLargestBlockSize)
    {
        ::operator delete(ptr);
        return;
    }

    poolFor(kPoolIndexOf(size)).deallocate(ptr);
}

} // namespace KamaMemoryPool