// expand_pool.cpp - 多 Chunk 自动扩容内存池 + 重载 operator new
// 编译：g++ expand_pool.cpp -o expand_pool -std=c++11 -Wall
#include <iostream>
#include <cstdlib>

class ExpandableMemPool {
public:
    ExpandableMemPool(size_t blockSize, size_t expandCnt)
        : m_blockSize(blockSize), m_expandCnt(expandCnt),
          m_freeList(nullptr), m_chunkList(nullptr)
    {
        expandChunk();
    }

    ~ExpandableMemPool() {
        ChunkHeader* cur = m_chunkList;
        while (cur) {
            ChunkHeader* next = cur->nextChunk;
            ::operator delete(cur);
            cur = next;
        }
    }

    void* allocate() {
        if (m_freeList == nullptr) expandChunk();
        char* ret  = m_freeList;
        m_freeList = *reinterpret_cast<char**>(m_freeList);
        return ret;
    }

    void deallocate(void* ptr) {
        if (!ptr) return;
        char* p       = static_cast<char*>(ptr);
        *reinterpret_cast<char**>(p) = m_freeList;
        m_freeList    = p;
    }

    ExpandableMemPool(const ExpandableMemPool&)            = delete;
    ExpandableMemPool& operator=(const ExpandableMemPool&) = delete;

private:
    struct ChunkHeader {
        ChunkHeader* nextChunk;
    };

    void expandChunk() {
        size_t total = sizeof(ChunkHeader) + m_expandCnt * m_blockSize;
        ChunkHeader* chunk = static_cast<ChunkHeader*>(::operator new(total));
        chunk->nextChunk   = m_chunkList;
        m_chunkList        = chunk;

        char* start = reinterpret_cast<char*>(chunk + 1);
        char* p     = start;
        for (size_t i = 0; i < m_expandCnt - 1; ++i) {
            *reinterpret_cast<char**>(p) = p + m_blockSize;
            p += m_blockSize;
        }
        *reinterpret_cast<char**>(p) = m_freeList;
        m_freeList = start;

        std::cout << "[扩容] 新建一个 Chunk，包含 " << m_expandCnt << " 个 Block\n";
    }

    size_t       m_blockSize;
    size_t       m_expandCnt;
    char*        m_freeList;
    ChunkHeader* m_chunkList;
};

struct TestObj {
    int    a;
    double b;

    TestObj(int _a, double _b) : a(_a), b(_b) {
        std::cout << "构造 TestObj  a=" << a << ", b=" << b << "\n";
    }
    ~TestObj() {
        std::cout << "析构 TestObj  a=" << a << ", b=" << b << "\n";
    }

    static ExpandableMemPool s_pool;

    static void* operator new(size_t size) {
        if (size != sizeof(TestObj)) throw std::bad_alloc();
        return s_pool.allocate();
    }
    static void operator delete(void* ptr) noexcept {
        s_pool.deallocate(ptr);
    }
};

ExpandableMemPool TestObj::s_pool(sizeof(TestObj), 5);

int main() {
    std::cout << "===== ExpandableMemPool 测试开始 =====\n";

    TestObj* p1 = new TestObj(1, 1.1);
    TestObj* p2 = new TestObj(2, 2.2);
    TestObj* p3 = new TestObj(3, 3.3);
    TestObj* p4 = new TestObj(4, 4.4);
    TestObj* p5 = new TestObj(5, 5.5);
    TestObj* p6 = new TestObj(6, 6.6);

    std::cout << "\n-- delete 前三个，Block 归还 --\n";
    delete p1;
    delete p2;
    delete p3;

    std::cout << "\n-- 复用归还的 Block 再次 new --\n";
    TestObj* p7 = new TestObj(7, 7.7);
    delete p7;

    std::cout << "\n-- 释放剩余对象 --\n";
    delete p4;
    delete p5;
    delete p6;

    std::cout << "===== 测试结束 =====\n";
    return 0;
}
//（注：内容由AI生成）
