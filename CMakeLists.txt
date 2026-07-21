cmake_minimum_required(VERSION 3.14)

project(cpp_logger VERSION 0.1.0 LANGUAGES CXX)

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
option(LOGGER_BUILD_SPDLOG_BENCHMARK "Build the cpp_logger versus spdlog benchmark" OFF)
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
    find_package(spdlog CONFIG QUIET)

    if(NOT TARGET spdlog::spdlog AND NOT TARGET spdlog::spdlog_header_only)
        if(NOT LOGGER_FETCH_SPDLOG)
            message(FATAL_ERROR
                "spdlog is required for logger_spdlog_benchmark. Install it, or configure with "
                "-DLOGGER_FETCH_SPDLOG=ON to fetch the pinned v1.17.0 dependency.")
        endif()

        include(FetchContent)
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.17.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(spdlog)
    endif()

    if(TARGET spdlog::spdlog_header_only)
        set(LOGGER_SPDLOG_TARGET spdlog::spdlog_header_only)
    elseif(TARGET spdlog::spdlog)
        set(LOGGER_SPDLOG_TARGET spdlog::spdlog)
    else()
        message(FATAL_ERROR "spdlog was found but did not export a supported CMake target")
    endif()

    add_executable(logger_spdlog_benchmark
        benchmark/comparison_benchmark.cpp
    )

    target_link_libraries(logger_spdlog_benchmark PRIVATE
        logger
        ${LOGGER_SPDLOG_TARGET}
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
