cmake_minimum_required(VERSION 3.14)

project(cpp_logger VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
include(CMakePackageConfigHelpers)
# 启用 CTest，并提供可由调用方关闭的 BUILD_TESTING 选项。
include(CTest)
include(GNUInstallDirs)

# 生成静态库
add_library(logger STATIC
    src/LogSink.cpp
    src/Logger.cpp
)

add_library(cpp_logger::logger ALIAS logger)

target_include_directories(logger PUBLIC # 凡是链接 logger 的目标，也会继承这个 include 路径
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# 头文件使用 C++17 特性；导出该要求后，消费者链接目标即可自动获得正确的语言标准。
target_compile_features(logger PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(logger PRIVATE /W4 /permissive-)
else()
    target_compile_options(logger PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(logger PUBLIC
    Threads::Threads
)

set(CPP_LOGGER_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/cpp_logger")

# 安装头文件、静态库和导出的目标，供其他项目通过 find_package() 使用。
install(TARGETS logger
    EXPORT cpp_loggerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/${PROJECT_NAME}
)

configure_package_config_file(
    cmake/cpp_loggerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cpp_loggerConfig.cmake
    INSTALL_DESTINATION ${CPP_LOGGER_CMAKE_INSTALL_DIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cpp_loggerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT cpp_loggerTargets
    FILE cpp_loggerTargets.cmake
    NAMESPACE cpp_logger::
    DESTINATION ${CPP_LOGGER_CMAKE_INSTALL_DIR}
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cpp_loggerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cpp_loggerConfigVersion.cmake
    DESTINATION ${CPP_LOGGER_CMAKE_INSTALL_DIR}
)


add_executable(logger_demo
    examples/example.cpp
)

target_link_libraries(logger_demo PRIVATE
    logger
)

option(LOGGER_BUILD_BENCHMARK "Build the logger performance benchmark" ON)
option(LOGGER_BUILD_QUILL_BENCHMARK "Build the cpp_logger versus Quill benchmark" OFF)
option(LOGGER_FETCH_QUILL "Fetch the pinned Quill dependency when it is not installed" OFF)
option(LOGGER_BUILD_SPDLOG_BENCHMARK "Build the standalone spdlog comparison benchmark" OFF)
option(LOGGER_FETCH_SPDLOG "Fetch the pinned spdlog dependency when it is not installed" OFF)

if(LOGGER_BUILD_BENCHMARK)
    add_executable(logger_benchmark
        benchmark/benchmark.cpp
    )

    target_link_libraries(logger_benchmark PRIVATE
        logger
    )
endif()

if(LOGGER_BUILD_SPDLOG_BENCHMARK)
    # spdlog 固定为 v1.17.0，且使用其 header-only 目标；这样基准的依赖构建方式可复现，
    # 不会把系统已安装静态库的编译选项混入测量结果。
    find_package(spdlog 1.17.0 EXACT CONFIG QUIET)

    if(NOT TARGET spdlog::spdlog_header_only)
        if(NOT LOGGER_FETCH_SPDLOG)
            message(FATAL_ERROR
                "spdlog v1.17.0 is required for the standalone benchmark. Install it, or configure with "
                "-DLOGGER_FETCH_SPDLOG=ON to fetch the pinned dependency.")
        endif()

        include(FetchContent)
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.17.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(spdlog)
    endif()

    if(NOT TARGET spdlog::spdlog_header_only)
        message(FATAL_ERROR "spdlog was found but did not export spdlog::spdlog_header_only")
    endif()

    add_executable(logger_spdlog_benchmark
        benchmark/spdlog_benchmark.cpp
    )
    target_link_libraries(logger_spdlog_benchmark PRIVATE
        spdlog::spdlog_header_only
    )
endif()

if(LOGGER_BUILD_QUILL_BENCHMARK)
    # 对照基准固定 Quill 版本，使不同运行环境的结果可追溯。
    find_package(quill 9.0.3 EXACT CONFIG QUIET)

    if(NOT TARGET quill::quill)
        if(NOT LOGGER_FETCH_QUILL)
            message(FATAL_ERROR
                "Quill is required for the logger Quill benchmark targets. Install it, or configure with "
                "-DLOGGER_FETCH_QUILL=ON to fetch the pinned v9.0.3 dependency.")
        endif()

        include(FetchContent)
        set(QUILL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(QUILL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(QUILL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
        # 避免 Quill 的 LOG_INFO 宏与项目对外提供的日志宏重名。
        set(QUILL_DISABLE_NON_PREFIXED_MACROS ON CACHE BOOL "" FORCE)
        # Quill v9 在 MinGW 环境下启用线程名称支持时，线程局部资源的清理会触发已知兼容性问题；
        # 对比程序不依赖线程名称，因此在该工具链下关闭它。
        if(MINGW)
            set(QUILL_NO_THREAD_NAME_SUPPORT ON CACHE BOOL "" FORCE)
        endif()
        FetchContent_Declare(
            quill
            GIT_REPOSITORY https://github.com/odygrd/quill.git
            GIT_TAG v9.0.3
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(quill)
    endif()

    if(NOT TARGET quill::quill)
        message(FATAL_ERROR "Quill was found but did not export quill::quill")
    endif()

    # Quill 约束同一线程只能使用一种 FrontendOptions；阻塞与丢新分别构建为独立可执行程序，
    # 避免两种队列类型在主线程的 flush 阶段混用。
    add_executable(logger_quill_blocking_benchmark
        benchmark/comparison_benchmark.cpp
    )
    target_compile_definitions(logger_quill_blocking_benchmark PRIVATE LOGGER_QUILL_QUEUE_MODE=1)
    target_link_libraries(logger_quill_blocking_benchmark PRIVATE
        logger
        quill::quill
    )

    add_executable(logger_quill_dropping_benchmark
        benchmark/comparison_benchmark.cpp
    )
    target_compile_definitions(logger_quill_dropping_benchmark PRIVATE LOGGER_QUILL_QUEUE_MODE=2)
    target_link_libraries(logger_quill_dropping_benchmark PRIVATE
        logger
        quill::quill
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
