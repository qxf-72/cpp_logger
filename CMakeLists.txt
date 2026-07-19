cmake_minimum_required(VERSION 3.10)

project(cpp_logger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
# 启用 CTest，并提供可由调用方关闭的 BUILD_TESTING 选项。
include(CTest)

# 生成静态库
add_library(logger STATIC
    src/Logger.cpp
)

target_include_directories(logger PUBLIC # 凡是链接 logger 的目标，也会继承这个 include 路径
    ${PROJECT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(logger PRIVATE /W4 /permissive-)
else()
    target_compile_options(logger PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(logger PUBLIC
    Threads::Threads
)


add_executable(logger_demo
    examples/example.cpp
)

target_link_libraries(logger_demo PRIVATE
    logger
)

option(LOGGER_BUILD_BENCHMARK "Build the logger performance benchmark" ON)

if(LOGGER_BUILD_BENCHMARK)
    add_executable(logger_benchmark
        benchmark/benchmark.cpp
    )

    target_link_libraries(logger_benchmark PRIVATE
        logger
    )
endif()

if(BUILD_TESTING)
    # 测试不依赖第三方框架，直接由 CTest 调用测试可执行程序。
    add_executable(logger_tests
        tests/logger_tests.cpp
    )

    target_link_libraries(logger_tests PRIVATE
        logger
    )

    add_test(NAME logger_tests COMMAND logger_tests)
    set_tests_properties(logger_tests PROPERTIES TIMEOUT 30)
endif()
