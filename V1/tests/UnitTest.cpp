#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include "MemoryPool.h"

using namespace KamaMemoryPool;

// ---------------------------------------------------------------------------
// 被测对象：大小不同的四类对象，覆盖多个定长池
// ---------------------------------------------------------------------------
class P1 { int field_; };
class P2 { int field_[5]; };
class P3 { int field_[10]; };
class P4 { int field_[20]; };

// ---------------------------------------------------------------------------
// 功能正确性测试：构造、析构、对齐、释放复用、大对象直通系统分配器
// ---------------------------------------------------------------------------
static void testCorrectness()
{
    bool ok = true;

    // 1. 各尺寸对象可正常 newElement / deleteElement（构造与析构都会被调用）
    P1* p1 = newElement<P1>();
    P2* p2 = newElement<P2>();
    P3* p3 = newElement<P3>();
    P4* p4 = newElement<P4>();
    deleteElement(p1);
    deleteElement(p2);
    deleteElement(p3);
    deleteElement(p4);

    // 2. 池内分配的对齐要求：Block 地址必须是其池 Block 大小的倍数
    P1* a = newElement<P1>();
    if (reinterpret_cast<std::uintptr_t>(a) % 8 != 0) ok = false;
    deleteElement(a);

    P4* b = newElement<P4>();
    if (reinterpret_cast<std::uintptr_t>(b) % 80 != 0) ok = false; // sizeof(P4) = 80
    deleteElement(b);

    // 3. 释放后复用：单线程下，刚释放的 Block 应被下一次分配直接取回
    P1* first  = newElement<P1>();
    P1* second = newElement<P1>();
    deleteElement(first);
    P1* reused = newElement<P1>();
    if (reused != first) ok = false; // 空闲链表栈顶复用
    deleteElement(reused);
    deleteElement(second);

    // 4. 大批量分配/释放的压力自检（覆盖多块 Chunk 的分配与越界检查）
    {
        std::vector<P1*> keep;
        for (int i = 0; i < 10000; ++i)
            keep.push_back(newElement<P1>());
        for (P1* p : keep)
            deleteElement(p);
        keep.clear();
    }

    // 5. 超过池内上限（512B）的对象直接使用系统分配器，行为保持一致
    struct Big { char data[1024]; };
    Big* big = newElement<Big>();
    deleteElement(big);

    std::cout << "[正确性测试] " << (ok ? "通过" : "失败") << std::endl;
}

// ---------------------------------------------------------------------------
// 性能对比：内存池 vs 系统 new/delete
// 单线程、多线程两种场景，输出各自总耗时（毫秒）
// ---------------------------------------------------------------------------
template <typename Func>
static double runBenchmark(std::size_t threadCount, std::size_t rounds, Func&& body)
{
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    for (std::size_t t = 0; t < threadCount; ++t)
        threads.emplace_back([&]() {
            for (std::size_t j = 0; j < rounds; ++j)
                body();
        });
    for (auto& th : threads)
        th.join();
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
}

static void benchmarkMemoryPool(std::size_t ntimes, std::size_t nworks, std::size_t rounds)
{
    const double cost = runBenchmark(nworks, rounds, [&]() {
        for (std::size_t i = 0; i < ntimes; ++i)
        {
            P1* p1 = newElement<P1>(); deleteElement(p1);
            P2* p2 = newElement<P2>(); deleteElement(p2);
            P3* p3 = newElement<P3>(); deleteElement(p3);
            P4* p4 = newElement<P4>(); deleteElement(p4);
        }
    });
    std::printf("[内存池] %zu 线程 x %zu 轮，每轮 %zu 次 newElement/deleteElement，总耗时 %.2f ms\n",
                nworks, rounds, ntimes, cost);
}

static void benchmarkNewDelete(std::size_t ntimes, std::size_t nworks, std::size_t rounds)
{
    const double cost = runBenchmark(nworks, rounds, [&]() {
        for (std::size_t i = 0; i < ntimes; ++i)
        {
            P1* p1 = new P1; delete p1;
            P2* p2 = new P2; delete p2;
            P3* p3 = new P3; delete p3;
            P4* p4 = new P4; delete p4;
        }
    });
    std::printf("[系统 new] %zu 线程 x %zu 轮，每轮 %zu 次 new/delete，总耗时 %.2f ms\n",
                nworks, rounds, ntimes, cost);
}

int main()
{
    HashBucket::initialize(); // 使用内存池前必须先初始化全部定长池

    testCorrectness();

    const std::size_t work = 1000, rounds = 20;
    benchmarkMemoryPool(work, 1, rounds);
    benchmarkNewDelete(work, 1, rounds);

    std::cout << "================================" << std::endl;

    benchmarkMemoryPool(work, 4, rounds);
    benchmarkNewDelete(work, 4, rounds);

    return 0;
}