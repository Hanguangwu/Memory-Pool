#pragma once

#include "Common.h"

#include <array>

namespace Kama_memoryPool
{

// ============================================================
// ThreadCache（线程缓存）：内存池的第一层
// 职责：
//   1. 每个线程独享的无锁空闲 Block 链表（按大小类划分）；
//   2. 快速分配：链表命中直接弹出，无锁 O(1)；
//   3. 慢速分配（链表空）：向 CentralCache 批量申请——一次领取一整批
//      （batchSize 个 Block，约 512 字节目标），而非像按需申请那样
//      每次只取 1 个；第一个直接返回给调用方，其余挂入线程本地链表，
//      后续分配不再触碰中央锁；
//   4. 快速释放：压入链表；超过保留阈值时把链表尾部多余 Block 归还 CentralCache。
// 线程安全：thread_local 单例，天然无竞争。
// ============================================================
class ThreadCache
{
public:
    static ThreadCache* getInstance(); // 每个线程一个实例

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);

private:
    ThreadCache(); // 清零空闲链表与计数

    // 向 CentralCache 批量领取一批 Block，返回本批第一个 Block
    void* fetchFromCentral(size_t index);

    // 超过保留阈值时，把链表尾部多余 Block 归还 CentralCache
    void returnToCentral(size_t index);

    // 每次向 CentralCache 领取的块数（目标约 512 字节，取值 2..64）
    static size_t batchSize(size_t index);

    // 线程内每个大小类最多保留的块数（约两批）
    static size_t keepLimit(size_t index);

private:
    std::array<void*,  FREE_LIST_SIZE> freeList_;  // 每个大小类的空闲链表头
    std::array<size_t, FREE_LIST_SIZE> freeCount_; // 每个大小类的空闲链表长度
};

} // namespace Kama_memoryPool