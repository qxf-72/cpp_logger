cmake_minimum_required(VERSION 3.10)

project(cpp_logger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# 生成静态库
add_library(logger STATIC
    src/Logger.cpp
)

target_include_directories(logger PUBLIC # 凡是链接 logger 的目标，也会继承这个 include 路径
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(logger PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(logger PUBLIC
    Threads::Threads
)


add_executable(logger_demo
    test/test.cpp
)

target_link_libraries(logger_demo PRIVATE
    logger
)
