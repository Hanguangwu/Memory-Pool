cmake_minimum_required(VERSION 3.10)

project(MemoryPoolProject CXX)

# ---- C++11 标准 ----
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 线程库（跨平台：Linux 链接 pthread，Windows 使用原生线程）----
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- 源文件 ----
file(GLOB SRC_FILES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB TEST_FILES CONFIGURE_DEPENDS "tests/*.cpp")

add_executable(${PROJECT_NAME} ${SRC_FILES} ${TEST_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE include)

target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# ---- 警告选项（按编译器区分）----
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
endif()