#include "../include/MemoryPool.h"
#include "../include/CentralCache.h"
#include "../include/PageCache.h"

#include <iostream>
#include <vector>
#include <thread>
#include <cassert>
#include <cstring>
#include <random>
#include <algorithm>
#include <atomic>

using namespace Kama_memoryPool;

// 遍历链表统计块数
static size_t countBlocks(void* head)
{
    size_t n = 0;
    for (void* cursor = head; cursor != nullptr;
         cursor = *reinterpret_cast<void**>(cursor))
    {
        ++n;
    }
    return n;
}

// 把 tailHead 整条链表接到 head 尾部（head 为 nullptr 时直接采用 tailHead）
static void appendList(void*& head, size_t& count, void* tailHead, size_t tailCount)
{
    if (tailHead == nullptr || tailCount == 0) return;
    void* tail = tailHead;
    for (size_t i = 1; i < tailCount; ++i)
    {
        tail = *reinterpret_cast<void**>(tail);
    }
    *reinterpret_cast<void**>(tail) = head;
    head = tailHead;
    count += tailCount;
}

// 基础分配测试
void testBasicAllocation() 
{
    std::cout << "Running basic allocation test..." << std::endl;
    
    // 测试小内存分配
    void* ptr1 = MemoryPool::allocate(8);
    assert(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 8);

    // 测试中等大小内存分配
    void* ptr2 = MemoryPool::allocate(1024);
    assert(ptr2 != nullptr);
    MemoryPool::deallocate(ptr2, 1024);

    // 测试大内存分配（超过MAX_BYTES）
    void* ptr3 = MemoryPool::allocate(1024 * 1024);
    assert(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, 1024 * 1024);

    std::cout << "Basic allocation test passed!" << std::endl;
}

// 内存写入测试
void testMemoryWriting() 
{
    std::cout << "Running memory writing test..." << std::endl;

    // 分配并写入数据
    const size_t size = 128;
    char* ptr = static_cast<char*>(MemoryPool::allocate(size));
    assert(ptr != nullptr);

    // 写入数据
    for (size_t i = 0; i < size; ++i) 
    {
        ptr[i] = static_cast<char>(i % 256);
    }

    // 验证数据
    for (size_t i = 0; i < size; ++i) 
    {
        assert(ptr[i] == static_cast<char>(i % 256));
    }

    MemoryPool::deallocate(ptr, size);
    std::cout << "Memory writing test passed!" << std::endl;
}

// 多线程测试
void testMultiThreading() 
{
    std::cout << "Running multi-threading test..." << std::endl;

    const int NUM_THREADS = 4;
    const int ALLOCS_PER_THREAD = 1000;
    std::atomic<bool> has_error{false};
    
    auto threadFunc = [&has_error, ALLOCS_PER_THREAD]() 
    {
        try 
        {
            std::vector<std::pair<void*, size_t>> allocations;
            allocations.reserve(ALLOCS_PER_THREAD);
            
            for (int i = 0; i < ALLOCS_PER_THREAD && !has_error; ++i) 
            {
                size_t size = (rand() % 256 + 1) * 8;
                void* ptr = MemoryPool::allocate(size);
                
                if (!ptr) 
                {
                    std::cerr << "Allocation failed for size: " << size << std::endl;
                    has_error = true;
                    break;
                }
                
                allocations.push_back({ptr, size});
                
                if (rand() % 2 && !allocations.empty()) 
                {
                    size_t index = rand() % allocations.size();
                    MemoryPool::deallocate(allocations[index].first, 
                                         allocations[index].second);
                    allocations.erase(allocations.begin() + index);
                }
            }
            
            for (const auto& alloc : allocations) 
            {
                MemoryPool::deallocate(alloc.first, alloc.second);
            }
        }
        catch (const std::exception& e) 
        {
            std::cerr << "Thread exception: " << e.what() << std::endl;
            has_error = true;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) 
    {
        threads.emplace_back(threadFunc);
    }

    for (auto& thread : threads) 
    {
        thread.join();
    }

    std::cout << "Multi-threading test passed!" << std::endl;
}

// 边界测试
void testEdgeCases() 
{
    std::cout << "Running edge cases test..." << std::endl;
    
    // 测试0大小分配
    void* ptr1 = MemoryPool::allocate(0);
    assert(ptr1 != nullptr);
    MemoryPool::deallocate(ptr1, 0);
    
    // 测试最小对齐大小
    void* ptr2 = MemoryPool::allocate(1);
    assert(ptr2 != nullptr);
    assert((reinterpret_cast<uintptr_t>(ptr2) & (ALIGNMENT - 1)) == 0);
    MemoryPool::deallocate(ptr2, 1);
    
    // 测试最大大小边界
    void* ptr3 = MemoryPool::allocate(MAX_BYTES);
    assert(ptr3 != nullptr);
    MemoryPool::deallocate(ptr3, MAX_BYTES);
    
    // 测试超过最大大小
    void* ptr4 = MemoryPool::allocate(MAX_BYTES + 1);
    assert(ptr4 != nullptr);
    MemoryPool::deallocate(ptr4, MAX_BYTES + 1);
    
    std::cout << "Edge cases test passed!" << std::endl;
}

// 压力测试
void testStress() 
{
    std::cout << "Running stress test..." << std::endl;

    const int NUM_ITERATIONS = 10000;
    std::vector<std::pair<void*, size_t>> allocations;
    allocations.reserve(NUM_ITERATIONS);

    for (int i = 0; i < NUM_ITERATIONS; ++i) 
    {
        size_t size = (rand() % 1024 + 1) * 8;
        void* ptr = MemoryPool::allocate(size);
        assert(ptr != nullptr);
        allocations.push_back({ptr, size});
    }

    // 随机顺序释放
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(allocations.begin(), allocations.end(), g);
    for (const auto& alloc : allocations) 
    {
        MemoryPool::deallocate(alloc.first, alloc.second);
    }

    std::cout << "Stress test passed!" << std::endl;
}

// 进程内第一次分配即触发批量预取：
// 全新进程（PageCache 空闲列表为空）的第一次分配只应产生一次系统调用
// —— PageCache 一次申请 PREFETCH_PAGES(128) 页，切出 8 页给 CentralCache，
// 其余 120 页作为空闲缓存。必须作为第一个测试运行（要求进程内此前无任何分配）。
void testFirstAllocationPrefetch()
{
    std::cout << "Running first-allocation prefetch test..." << std::endl;

    PageCache& pc = PageCache::getInstance();
    const size_t before = pc.getSystemAllocCount(); // 进程内尚未发生任何系统分配
    assert(before == 0);

    void* p = MemoryPool::allocate(16);
    assert(p != nullptr);
    const size_t after = pc.getSystemAllocCount(); // 批量预取：一次系统调用拿到一整批
    assert(after == before + 1);
    std::cout << "  first allocation -> " << (after - before)
              << " system alloc call(s) (PREFETCH_PAGES="
              << PageCache::PREFETCH_PAGES << ")" << std::endl;

    MemoryPool::deallocate(p, 16);
    std::cout << "First-allocation prefetch test passed!" << std::endl;
}

// PageCache 批量预取测试：
// 多次小页请求应复用预取缓存，向操作系统的申请次数不随请求数线性增长。
// 全部归还后相邻 Chunk 合并回同一大块，也不产生新的系统调用。
void testPageCacheBatchPrefetch()
{
    std::cout << "Running PageCache batch prefetch test..." << std::endl;

    PageCache& pc = PageCache::getInstance();
    const size_t before = pc.getSystemAllocCount();

    const size_t N = 16; // 16 次 1 页请求
    std::vector<Chunk*> chunks;
    chunks.reserve(N);
    for (size_t i = 0; i < N; ++i)
    {
        chunks.push_back(pc.allocateChunk(1));
        assert(chunks.back() != nullptr);
    }

    // 批量预取效果：第一次请求一次性向 OS 申请 PREFETCH_PAGES(128) 页，
    // 其余 15 次请求全部命中空闲缓存，因此新增系统调用至多 1 次
    const size_t delta = pc.getSystemAllocCount() - before;
    assert(delta <= 1);
    std::cout << "  " << N << " x 1-page requests -> " << delta
              << " system alloc call(s)" << std::endl;

    for (size_t i = 0; i < N; ++i)
    {
        pc.deallocateChunk(chunks[i]);
    }

    // 归还不触发新的系统调用（只合并/挂回空闲列表）
    assert(pc.getSystemAllocCount() == before + delta);

    std::cout << "PageCache batch prefetch test passed!" << std::endl;
}

// CentralCache 批量发放测试：
// 当某大小类的中央链表耗尽时，下一次批量领取必须新建 Chunk 并整批
// （满 batchNum 个 Block）返回——这正是 ThreadCache 慢速路径拿到的
// 「一批而非一个」的保证。
void testCentralCacheBatchFetch()
{
    std::cout << "Running CentralCache batch fetch test..." << std::endl;

    CentralCache& cc = CentralCache::getInstance();
    const size_t index = SizeClass::getIndex(512); // 512B 类：8 页 Chunk = 64 块
    const size_t BATCH = 64;                       // 与 batchSize(512B 类) 一致
    const size_t DRAIN = 4096;                     // 大于任何单个 Chunk 的块数

    // 1) 清空中央链表：每轮批量领取 DRAIN 个；返回不足 DRAIN 个即链表已空。
    //    链表为空时 fetchBlocks 会新建 Chunk 返回 64 个（< DRAIN），同样触底。
    //    轮数上限 4 为防御（4 x 4096 块远超本测试套件的使用规模）。
    void*  drained  = nullptr;
    size_t drainedN = 0;
    for (int round = 0; round < 4; ++round)
    {
        void* b = cc.fetchBlocks(index, DRAIN);
        if (b == nullptr) break; // 链表恰为空
        size_t n = countBlocks(b);
        appendList(drained, drainedN, b, n);
        if (n < DRAIN) break; // 链表已空
    }

    // 2) 链表已空：批量领取必须新建 Chunk，并整批返回 BATCH 个 Block
    void* batch = cc.fetchBlocks(index, BATCH);
    assert(batch != nullptr);
    assert(countBlocks(batch) == BATCH);

    // 3) 整批归还：串回已取出的链表，恢复记账平衡
    appendList(drained, drainedN, batch, BATCH);
    cc.returnBlocks(drained, drainedN, index);

    std::cout << "CentralCache batch fetch test passed!" << std::endl;
}

int main() 
{
    try 
    {
        std::cout << "Starting memory pool tests..." << std::endl;

        testFirstAllocationPrefetch(); // 必须最先运行：验证进程内首次分配即批量预取
        testBasicAllocation();
        testMemoryWriting();
        testMultiThreading();
        testEdgeCases();
        testStress();
        testPageCacheBatchPrefetch();
        testCentralCacheBatchFetch();

        std::cout << "All tests passed successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) 
    {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}