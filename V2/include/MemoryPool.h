#pragma once

#include "ThreadCache.h"

namespace Kama_memoryPool
{

// ============================================================
// MemoryPool：对外统一入口，转发给线程本地缓存
// ============================================================
class MemoryPool
{
public:
    static void* allocate(size_t size)
    {
        return ThreadCache::getInstance()->allocate(size);
    }

    static void deallocate(void* ptr, size_t size)
    {
        ThreadCache::getInstance()->deallocate(ptr, size);
    }
};

} // namespace Kama_memoryPool