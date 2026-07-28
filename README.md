<div align="center">

# cpp_logger

**基于 C++17 的轻量级异步日志库**

[English](README_EN.md) | 简体中文

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B)
![CMake](https://img.shields.io/badge/CMake-3.14%2B-064F8C?logo=cmake)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-555555?logo=linux&logoColor=FCC624)
[![License](https://img.shields.io/github/license/qxf-72/cpp_logger?color=yellow)](LICENSE)
[![CI](https://github.com/qxf-72/cpp_logger/actions/workflows/ci.yml/badge.svg)](https://github.com/qxf-72/cpp_logger/actions/workflows/ci.yml)
![Release](https://img.shields.io/github/v/release/qxf-72/cpp_logger?display_name=tag&logo=github)
[![Stars](https://img.shields.io/github/stars/qxf-72/cpp_logger?style=flat)](https://github.com/qxf-72/cpp_logger/stargazers)

</div>

`cpp_logger` 让业务线程快速提交 `LogRecord`，由后台线程统一完成格式化、批量写入和文件滚动。它适合作为学习 C++ 并发与异步 I/O 的完整项目，也可作为轻量级文件日志组件直接集成到 CMake 工程。

> 适用于本地文件日志与并发程序的可观测性；不以分布式日志或断电级持久化为目标。

## ✨ 核心能力

| 能力 | 说明 |
| --- | --- |
| 异步批量写入 | 将格式化与文件 I/O 移出业务线程；后台按批出队、格式化并写入。 |
| 有界队列与过载控制 | `Block`、`DropNewest`、`DropOldest` 三种队列满策略。 |
| 可观测性 | `droppedCount()`、`queueSize()`、`queuePeakSize()` 用于监控过载情况。 |
| 日志管理 | 五级过滤、毫秒时间戳、线程 ID、源位置、按日期/大小滚动。 |
| 输出端扩展 | 内置 `FileSink`、`ConsoleSink`，可接入自定义 `LogSink`。 |
| 工程化交付 | CTest、跨平台 GitHub Actions、格式化/静态分析/Sanitizer 门禁、CMake 安装与 `find_package` 包导出。 |

## 🧭 导航

- [快速开始](#-快速开始)
- [使用与配置](#-使用与配置)
- [核心数据流](#️-核心数据流)
- [测试、CI 与安装](#-测试ci-与安装)
- [性能测试](#-性能测试)

## 🚀 快速开始

### 环境要求

- C++17 编译器：GCC、Clang 或 MSVC
- CMake 3.14 或更高版本
- 推荐 Ninja；也可替换为本机可用的 CMake 生成器

从源码构建并运行测试：

```bash
git clone https://github.com/qxf-72/cpp_logger.git
cd cpp_logger

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

验证构建结果：

```bash
ctest --test-dir build --output-on-failure
```

运行示例程序：

```bash
# Linux / macOS
./build/logger_demo

# Windows PowerShell
.\build\logger_demo.exe
```

日志默认写入 `logs/` 目录。

## 🧩 使用与配置

```cpp
#include <chrono>
#include <iostream>

#include "Logger.h"

int main() {
  LoggerConfig config;
  config.basePath = "logs/app";
  config.minLevel = LogLevel::DEBUG;
  config.maxFileSize = 10 * 1024 * 1024;
  config.queueCapacity = 8192;
  config.overflowPolicy = OverflowPolicy::Block;
  config.writeBatchSize = 256;
  config.flushPolicy = FlushPolicy::Periodic;
  config.flushInterval = std::chrono::seconds(1);
  config.flushAtOrAbove = LogLevel::ERROR;
  config.enableConsoleSink = true;
  config.consoleStream = ConsoleStream::Stdout;

  if (!Logger::instance().init(config)) {
    return 1;
  }

  LOG_DEBUG("debug message");
  LOG_INFO("program started");
  LOG_WARN("warning message");
  LOG_ERROR("error message");

  std::cout << "dropped=" << Logger::instance().droppedCount() << '\n';
  Logger::instance().stop();
}
```

`LOG_DEBUG`、`LOG_INFO`、`LOG_WARN`、`LOG_ERROR` 和 `LOG_FATAL` 会先检查日志级别，避免被过滤的日志构造不必要的消息。请在应用的顶层生命周期中显式调用 `stop()`；它负责排空已接收记录并关闭输出端。旧接口 `init("app", LogLevel::DEBUG, 10 * 1024 * 1024)` 仍可用。

### 队列满时的策略

| 策略 | 行为 | 适用场景 |
| --- | --- | --- |
| `OverflowPolicy::Block` | 阻塞生产者，直到后台线程取走日志或日志器关闭。 | 日志不能丢失。 |
| `OverflowPolicy::DropNewest` | 丢弃本次新日志并立即返回。 | 优先保障业务线程延迟。 |
| `OverflowPolicy::DropOldest` | 丢弃队列中最早的日志，保留最新日志。 | 排障时更关注最新状态。 |

`droppedCount()` 统计两种丢弃策略造成的日志损失；`queueSize()` 返回当前待写条数；`queuePeakSize()` 返回当前初始化周期内的队列峰值。每次成功 `init()` 都会重置这些统计值。

### 常用配置

| 配置 | 默认值 | 说明 |
| --- | ---: | --- |
| `queueCapacity` | `8192` | 有界队列容量（记录数）。 |
| `writeBatchSize` | `256` | 单次最多格式化并写入的记录数。 |
| `flushPolicy` | `Periodic` | `OnStop`、`Periodic`、`EveryBatch`。 |
| `flushInterval` | `1 s` | `Periodic` 模式的刷新周期。 |
| `flushAtOrAbove` | `ERROR` | 到达该级别及以上时，在当前批次内刷新。 |
| `maxFileSize` | `1 MiB` | 单个日志文件的最大大小。 |

`FileSink` 默认启用并负责按日期/大小滚动；`ConsoleSink` 可选，两个内置 Sink 可以同时启用。关闭文件输出后，`basePath` 不再是必填项；自定义 Sink 继承 `LogSink` 并放入 `additionalSinks` 即可。

> `flush()` 使用 `std::ofstream::flush()` 将 C++ 流缓冲刷新到操作系统，不等同于 `fsync`，因此不提供断电安全的持久化保证。

## 🏗️ 核心数据流

```mermaid
flowchart LR
    P[业务线程] --> R[采集 LogRecord]
    R --> Q[有界 BlockingQueue]
    Q --> W[后台日志线程]
    W --> F[批量格式化]
    F --> S[FileSink / ConsoleSink / 自定义 Sink]
    S --> D[批量输出]
```

- 业务线程采集时间、级别、线程 ID、源位置和消息后入队；字符串格式化延后到后台线程。
- `stop()` 拒绝后续写入、排空已接收记录、刷新并关闭全部 Sink。
- 生命周期同步与队列代次避免旧生命周期的生产者在重新初始化后写入新队列。

日志行格式：

```text
[2026-06-25 12:00:00.123][INFO][tid:140123456789000][example.cpp:42] message
```

日志文件名为 `<前缀>_<日期>_<序号>.log`，例如 `app_2026-06-25_0.log`。

## ✅ 测试、CI 与安装

每次推送到 `main` 或创建 Pull Request 时，GitHub Actions 会在 Windows、Linux、macOS 上执行配置、构建、CTest 和 CMake 安装验证；并在 Linux 上执行以下质量门禁：

- 使用 `clang-format-18` 对所有受版本控制的 C++ 源文件进行只读格式检查；
- 以 Clang 编译并启用 `.clang-tidy` 中的缺陷、性能与可读性规则，任何已启用诊断都会令构建失败；
- 分别以 AddressSanitizer（ASan）和 ThreadSanitizer（TSan）插桩构建并运行 CTest。

本地仅运行测试：

```bash
ctest --test-dir build --output-on-failure
```

测试覆盖队列生命周期、满队列策略、初始化参数校验、日志过滤、安全停止排空、文件滚动、重新初始化和多线程写入。

### 本地复现质量检查

`LOGGER_ENABLE_CLANG_TIDY` 会在构建各项目标时运行静态分析。`LOGGER_ENABLE_ASAN` 与 `LOGGER_ENABLE_TSAN` 互斥，当前固定用于 Linux + Clang/GNU 环境；它们只影响本项目构建，不会传播给安装包消费者。

```bash
# clang-format（Bash / Git Bash）
git ls-files -z -- '*.cpp' '*.h' '*.hpp' | xargs -0 clang-format --dry-run --Werror --style=file

# clang-tidy
cmake -S . -B build-clang-tidy -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTING=ON -DLOGGER_ENABLE_CLANG_TIDY=ON
cmake --build build-clang-tidy --parallel
```

在 Linux 上复现 ASan（将 `ASAN` 改为 `TSAN` 即可运行另一套检查，二者不可同时开启）：

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTING=ON -DLOGGER_BUILD_BENCHMARK=OFF -DLOGGER_ENABLE_ASAN=ON
cmake --build build-asan --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

### 作为 CMake 包使用

安装静态库、头文件和 CMake package：

```bash
cmake --install build --prefix <安装目录>
```

在消费者项目中：

```cmake
find_package(cpp_logger 0.1 CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE cpp_logger::logger)
```

配置消费者项目时指定安装目录：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<安装目录>
```

GitHub Release 提供 Windows MSVC x64、Linux GCC x64 和 macOS AppleClang arm64 的预构建包，以及 `SHA256SUMS.txt` 校验文件。预构建静态库不能跨平台或跨编译器混用。

## 📊 性能测试

常规压测分别统计生产者提交吞吐量，以及 `stop()` 排空、刷新完成后的端到端吞吐量：

```bash
# Linux / macOS
./build/logger_benchmark --threads 4 --messages 50000 --payload 128 --runs 5

# Windows PowerShell
.\build\logger_benchmark.exe --threads 4 --messages 50000 --payload 128 --runs 5
```

| 参数 | 说明 |
| --- | --- |
| `--threads <N>` | 生产者线程数。 |
| `--messages <N>` | 每个生产者提交的日志数。 |
| `--payload <N>` | 单条日志正文大小（字节）。 |
| `--runs <N>` | 重复轮数。 |
| `--batch-size <N>` | 后台线程单批记录数。 |
| `--flush-policy <...>` | `on-stop`、`periodic` 或 `every-batch`。 |

一次本机 Release 测试中，`cpp_logger` 使用 Windows、MSYS2 MinGW-w64 GCC 15.2.0，4 个生产者、每线程 250,000 条、128 B 正文、`Block + Periodic 1 s`、3 轮平均，端到端吞吐约为 **123.80 万条/秒**。

<details>
<summary><strong>展开查看不同实现与工具链下的参考数据、复现命令和完整结果</strong></summary>

第三方对照目标默认关闭，不会影响常规构建。Quill 固定为 v9.0.3，spdlog 固定为 v1.17.0。

```bash
# Quill：Ninja / Release
cmake -S . -B build-quill -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_QUILL_BENCHMARK=ON -DLOGGER_FETCH_QUILL=ON
cmake --build build-quill --target logger_quill_blocking_benchmark logger_quill_dropping_benchmark --parallel

# spdlog：Windows MSVC x64 / Release
cmake -S . -B build-spdlog-msvc -G "Visual Studio 17 2022" -A x64 -DLOGGER_BUILD_SPDLOG_BENCHMARK=ON -DLOGGER_FETCH_SPDLOG=ON
cmake --build build-spdlog-msvc --config Release --target logger_spdlog_benchmark --parallel
```

Quill 的可靠与丢新 Profile 必须使用独立可执行程序；spdlog 的 `Block` 和 `DiscardNew` 分别对齐可靠与丢新语义。`DropOldest` / `OverrunOldest` 未纳入表格，因为没有同一轮、同一工具链下的等价对照数据。

`cpp_logger` 与 Quill 行来自 2026-07-26 的 Windows、MSYS2 MinGW-w64 GCC 15.2.0、Release 测试；spdlog 行来自 2026-07-27 的 Windows、Visual Studio 2022 x64、MSVC 19.40.33811、Release 测试。每个配置均为 3 轮、每生产者 250,000 条、128 B 正文、1 秒周期刷新。由于编译器、队列拓扑、格式化器和 Sink 都不同，下表只能描述特定配置，**不能用作严格的跨库排名**。

#### 4 个生产者

| 场景 / 实现 | 队列策略 | 生产者吞吐量（万条/秒） | 端到端吞吐量（万条/秒） | 丢弃率 |
| --- | --- | ---: | ---: | ---: |
| 可靠 / 周期刷新（cpp_logger） | Block | 124.92 | 123.80 | 0.0000% |
| 可靠 / 周期刷新（spdlog，MSVC） | Block | 48.30 | 48.16 | 0.0000% |
| 可靠 / 周期刷新（Quill） | UnboundedBlocking | 874.92 | 80.53 | 0.0000% |
| 容忍丢失 / 丢新（cpp_logger） | DropNewest | 309.61 | 120.15 | 60.3628% |
| 容忍丢失 / 丢新（spdlog，MSVC） | DiscardNew | 460.29 | 76.98 | 82.7957% |
| 容忍丢失 / 丢新（Quill） | BoundedDropping | 5273.86 | 80.30 | 97.4055% |

#### 8 个生产者

| 场景 / 实现 | 队列策略 | 生产者吞吐量（万条/秒） | 端到端吞吐量（万条/秒） | 丢弃率 |
| --- | --- | ---: | ---: | ---: |
| 可靠 / 周期刷新（cpp_logger） | Block | 103.37 | 102.60 | 0.0000% |
| 可靠 / 周期刷新（spdlog，MSVC） | Block | 33.91 | 33.83 | 0.0000% |
| 可靠 / 周期刷新（Quill） | UnboundedBlocking | 943.82 | 73.11 | 0.0000% |
| 容忍丢失 / 丢新（cpp_logger） | DropNewest | 230.12 | 103.53 | 54.2425% |
| 容忍丢失 / 丢新（spdlog，MSVC） | DiscardNew | 428.28 | 47.40 | 88.6532% |
| 容忍丢失 / 丢新（Quill） | BoundedDropping | 5906.90 | 65.62 | 97.8928% |

#### 16 个生产者

| 场景 / 实现 | 队列策略 | 生产者吞吐量（万条/秒） | 端到端吞吐量（万条/秒） | 丢弃率 |
| --- | --- | ---: | ---: | ---: |
| 可靠 / 周期刷新（cpp_logger） | Block | 72.30 | 71.62 | 0.0000% |
| 可靠 / 周期刷新（spdlog，MSVC） | Block | 33.77 | 33.70 | 0.0000% |
| 可靠 / 周期刷新（Quill） | UnboundedBlocking | 843.36 | 68.28 | 0.0000% |
| 容忍丢失 / 丢新（cpp_logger） | DropNewest | 123.35 | 36.84 | 69.7742% |
| 容忍丢失 / 丢新（spdlog，MSVC） | DiscardNew | 463.51 | 26.23 | 94.1834% |
| 容忍丢失 / 丢新（Quill） | BoundedDropping | 6194.27 | 46.62 | 98.3951% |

- 丢新策略的高提交速率伴随较高丢弃率，不能视为可靠写入能力。
- 选择日志库或过载策略时，应同时考察端到端吞吐、最终保留量、延迟与业务可接受的丢失上限。

</details>

## 🗺️ 后续计划

- [x] 支持控制台与可插拔输出端
- [x] 建立 clang-format、clang-tidy、ASan 与 TSan 质量门禁
- [ ] 支持日志文件保留期与自动清理

## 🤝 贡献

欢迎提交 Issue 和 Pull Request。提交前请至少运行：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 📄 许可证

本项目采用 [MIT License](LICENSE)。
