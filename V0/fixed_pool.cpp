// fixed_pool.cpp - 单 Chunk 固定预分配内存池
// 编译：g++ fixed_pool.cpp -o fixed_pool -std=c++11 -Wall
#include <iostream>
#include <cstring>

class FixedMemPool {
public:
    FixedMemPool(size_t blockSize, size_t blockNum)
        : m_blockSize(blockSize), m_blockNum(blockNum)
    {
        m_chunkStart = static_cast<char*>(::operator new(blockSize * blockNum));
        m_freeList   = m_chunkStart;

        char* p = m_chunkStart;
        for (size_t i = 0; i < blockNum - 1; ++i) {
            *reinterpret_cast<char**>(p) = p + blockSize;
            p += blockSize;
        }
        *reinterpret_cast<char**>(p) = nullptr;
    }

    ~FixedMemPool() {
        ::operator delete(m_chunkStart);
    }

    FixedMemPool(const FixedMemPool&)            = delete;
    FixedMemPool& operator=(const FixedMemPool&) = delete;

    void* allocate() {
        if (m_freeList == nullptr) {
            std::cerr << "[FixedMemPool] 内存池已耗尽！\n";
            return nullptr;
        }
        char* ret  = m_freeList;
        m_freeList = *reinterpret_cast<char**>(m_freeList);
        return ret;
    }

    void deallocate(void* ptr) {
        if (ptr == nullptr) return;
        char* p        = static_cast<char*>(ptr);
        *reinterpret_cast<char**>(p) = m_freeList;
        m_freeList     = p;
    }

private:
    char*  m_chunkStart;
    char*  m_freeList;
    size_t m_blockSize;
    size_t m_blockNum;
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
};

int main() {
    size_t blockSize = sizeof(TestObj) > sizeof(char*) ? sizeof(TestObj)
                                                      : sizeof(char*);
    FixedMemPool pool(blockSize, 5);
    std::cout << "===== FixedMemPool 测试开始 =====\n";

    TestObj* o1 = new (pool.allocate()) TestObj(1, 1.1);
    TestObj* o2 = new (pool.allocate()) TestObj(2, 2.2);

    o1->~TestObj();
    pool.deallocate(o1);
    o2->~TestObj();
    pool.deallocate(o2);

    std::cout << "===== 测试结束 =====\n";
    return 0;
}
//（注：内容由AI生成）
