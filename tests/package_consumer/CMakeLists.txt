cmake_minimum_required(VERSION 3.14)

project(cpp_logger_package_consumer LANGUAGES CXX)

include(CTest)

# 故意设置为 C++11：链接导出的目标后，应由其 PUBLIC 要求自动提升至 C++17。
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(cpp_logger 0.1 CONFIG REQUIRED)

add_executable(cpp_logger_package_consumer
    main.cpp
)

target_link_libraries(cpp_logger_package_consumer PRIVATE
    cpp_logger::logger
)

if(BUILD_TESTING)
    add_test(NAME cpp_logger_package_consumer COMMAND cpp_logger_package_consumer)
endif()
