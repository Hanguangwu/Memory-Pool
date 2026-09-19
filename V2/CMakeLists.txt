cmake_minimum_required(VERSION 3.10)
project(memory_pool)

# C++11 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项（MSVC 与 GCC/Clang 分开设置）
if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# 线程库（Windows 上为无操作，POSIX 上链接 pthread）
find_package(Threads REQUIRED)

set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_SOURCE_DIR}/include)

# 内存池核心源文件
file(GLOB SOURCES "${SRC_DIR}/*.cpp")
include_directories(${INC_DIR})

# 单元测试
add_executable(unit_test
    ${SOURCES}
    ${CMAKE_SOURCE_DIR}/tests/UnitTest.cpp
)

# 性能测试
add_executable(perf_test
    ${SOURCES}
    ${CMAKE_SOURCE_DIR}/tests/PerformanceTest.cpp
)

target_link_libraries(unit_test PRIVATE Threads::Threads)
target_link_libraries(perf_test PRIVATE Threads::Threads)

# 用 ctest 运行：ctest 运行全部测试；ctest -R perf 只运行性能测试
enable_testing()
add_test(NAME unit_test COMMAND unit_test)
add_test(NAME perf COMMAND perf_test)