cmake_minimum_required(VERSION 3.10)

project(cpp_logger)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(logger_demo
    src/main.cpp
    src/Logger.cpp
)

target_include_directories(logger_demo PRIVATE include)

target_link_libraries(logger_demo PRIVATE Threads::Threads)