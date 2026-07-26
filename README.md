<div align="center">

# cpp_logger

**基于 C++17 的轻量级异步日志库**

[English](README_EN.md) | 简体中文

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B)
![CMake](https://img.shields.io/badge/CMake-3.14%2B-064F8C?logo=cmake)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-555555?logo=linux&logoColor=FCC624)
[![CI](https://github.com/qxf-72/cpp_logger/actions/workflows/ci.yml/badge.svg)](https://github.com/qxf-72/cpp_logger/actions/workflows/ci.yml)
![Release](https://img.shields.io/github/v/release/qxf-72/cpp_logger?display_name=tag&logo=github)
[![License](https://img.shields.io/github/license/qxf-72/cpp_logger?color=yellow)](LICENSE)
![Stars](https://img.shields.io/github/stars/qxf-72/cpp_logger?logo=github&label=stars&color=F5C518)

</div>

`cpp_logger` 将日志记录先写入有界队列，再由后台线程批量格式化、批量写入文件。它提供可选的队列过载策略、文件滚动、刷新策略、运行统计、CTest 测试和 CMake 包导出，适合作为 C++ 并发与异步 I/O 的学习型项目，也可作为轻量级文件日志组件使用。

## ✨ 特性

| 能力 | 说明 |
| --- | --- |
| 异步写入 | 业务线程提交 `LogRecord`；后台线程负责格式化、滚动和分发至输出端。 |
| 有界队列 | 支持 `Block`、`DropNewest`、`DropOldest` 三种队列满策略。 |
| 批量处理 | 按批出队、格式化和写入，减少同步与流写入次数。 |
| 可观测性 | 提供 `droppedCount()`、`queueSize()` 与 `queuePeakSize()`。 |
| 日志管理 | 五级过滤、毫秒时间戳、线程 ID、源文件位置、按日期/大小滚动。 |
| 刷新与停止 | 支持停止时、定时、每批刷新；`stop()` 会排空已接收的日志。 |
| 多输出端 | 内置滚动 `FileSink` 与 `ConsoleSink`，也可接入自定义 `LogSink`。 |
| 工程化 | 支持 CTest、跨平台 GitHub Actions CI、`find_package` 包导出。 |

## 🚀 快速开始

### 环境要求

- C++17 编译器：GCC、Clang 或 MSVC
- CMake 3.14 或更高版本
- 推荐使用 Ninja；也可替换为本机可用的 CMake 生成器

以下命令以 Ninja 为例：

```bash
git clone https://github.com/qxf-72/cpp_logger.git
cd cpp_logger

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
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

## 🧩 使用方式

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

`LOG_DEBUG`、`LOG_INFO`、`LOG_WARN`、`LOG_ERROR` 和 `LOG_FATAL` 会在构造消息前先检查日志级别，避免被过滤日志产生不必要的字符串开销。

旧接口 `init("app", LogLevel::DEBUG, 10 * 1024 * 1024)` 仍可用；它使用默认队列容量 `8192`、`Block` 策略、256 条批处理与定时刷新。

### 队列满时的策略

| 策略 | 行为 | 适用场景 |
| --- | --- | --- |
| `OverflowPolicy::Block` | 阻塞生产者，直到后台线程取走日志或日志器关闭。 | 不能接受日志丢失。 |
| `OverflowPolicy::DropNewest` | 立即丢弃本次新日志并返回。 | 优先保障业务线程延迟。 |
| `OverflowPolicy::DropOldest` | 丢弃队列中最早的日志，保留本次新日志。 | 排障时更关注最新状态。 |

`droppedCount()` 统计两种丢弃策略造成的日志丢失；`queueSize()` 返回当前待写条数；`queuePeakSize()` 返回本次初始化周期内的队列峰值。每次成功 `init()` 都会重置这些统计值。

### 批处理与刷新

| 配置 | 含义 |
| --- | --- |
| `writeBatchSize` | 单次最多格式化并写入的记录数，默认 `256`。 |
| `FlushPolicy::OnStop` | 仅在 `stop()` 时刷新；但达到 `flushAtOrAbove` 的记录仍会触发刷新。 |
| `FlushPolicy::Periodic` | 按 `flushInterval` 定时刷新，默认 1 秒；也是默认策略。 |
| `FlushPolicy::EveryBatch` | 每个写入批次后刷新，日志可见性更快但开销更高。 |

`flushAtOrAbove` 默认是 `LogLevel::ERROR`；设置为 `std::nullopt` 可关闭按级别刷新。这里的 `FileSink` 使用的 `std::ofstream::flush()` 将 C++ 流缓冲刷新到操作系统，并不等同于 `fsync`，因此不提供断电安全的持久化保证。

### 输出端（Sink）

`FileSink` 默认启用，负责按日期和大小滚动文件；`ConsoleSink` 可选，并可写入标准输出或标准错误。两个内置 Sink 可同时启用，接收同一批格式化日志：

```cpp
config.enableFileSink = true;               // 默认值；启用时 basePath 必填
config.enableConsoleSink = true;
config.consoleStream = ConsoleStream::Stderr;
```

关闭文件输出后，可以仅使用控制台或自定义输出端，此时 `basePath` 不再必填。自定义 Sink 继承 `LogSink` 并放入 `additionalSinks`；其 `writeBatch()`、`flush()` 和 `close()` 均由日志后台线程调用。

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

- 业务线程采集时间、级别、线程 ID、源位置与消息后入队；字符串格式化延后到后台线程。
- `close()` 会唤醒等待中的线程；`stop()` 拒绝后续写入、排空已接收记录，再刷新并关闭文件。
- 重新初始化时通过生命周期同步与队列代次避免旧生命周期的生产者写入新队列。

日志格式如下：

```text
[2026-06-25 12:00:00.123][INFO][tid:140123456789000][example.cpp:42] message
```

文件名格式为 `<前缀>_<日期>_<序号>.log`，例如 `app_2026-06-25_0.log`。日期变化或文件达到 `maxFileSize` 时会创建新文件。

## ✅ 测试与 CI

每次推送到 `main` 或创建 Pull Request 时，GitHub Actions 会在 Windows、Linux 和 macOS 上执行：配置、构建、CTest 和 CMake 安装验证。

本地运行测试：

```bash
ctest --test-dir build --output-on-failure
```

测试覆盖队列生命周期和满队列策略、初始化参数校验、日志级别过滤、安全停止排空、大小滚动、重新初始化以及多线程写入。

## 📦 安装与 CMake 包导出

构建完成后可安装静态库、头文件与 CMake package：

```bash
cmake --install build --prefix <安装目录>
```

在其他 CMake 项目中使用：

```cmake
find_package(cpp_logger 0.1 CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE cpp_logger::logger)
```

配置消费者项目时指定安装目录：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<安装目录>
```

### 从 GitHub Release 使用预构建包

从 `v0.1.1` 起，推送与 CMake 项目版本一致的 `vX.Y.Z` 标签会自动构建并发布预构建 CMake 包。下载与本机平台、编译器和架构匹配的附件并解压后，无需克隆本仓库：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<解压后的 cpp_logger 目录>
cmake --build build --parallel
```

可用附件包括 Windows MSVC x64、Linux GCC x64 和 macOS AppleClang arm64。每个 Release 都会附带 `SHA256SUMS.txt`，可用于校验下载内容；预构建静态库不能跨平台或跨编译器混用。

## 📊 性能测试

常规压测目标默认开启，输出 CSV，分别报告生产者提交吞吐量与 `stop()` 排空、刷新后的端到端吞吐量：

```bash
# Linux / macOS
./build/logger_benchmark --threads 4 --messages 50000 --payload 128 --runs 5

# Windows PowerShell
.\build\logger_benchmark.exe --threads 4 --messages 50000 --payload 128 --runs 5
```

| 参数 | 说明 |
| --- | --- |
| `--threads <N>` | 生产者线程数，默认 4。 |
| `--messages <N>` | 每个生产者写入的日志数，默认 50000。 |
| `--payload <N>` | 单条日志正文大小（字节），默认 128。 |
| `--runs <N>` | 重复轮数，默认 3。 |
| `--batch-size <N>` | 后台线程单批记录数，默认 256。 |
| `--flush-policy <...>` | `on-stop`、`periodic` 或 `every-batch`。 |

可选的第三方对照压测固定使用 Quill v9.0.3 与 spdlog v1.17.0；默认关闭，不会影响常规构建。若本机没有对应精确版本，可由 CMake 下载：

```bash
cmake -S . -B build-quill -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_QUILL_BENCHMARK=ON -DLOGGER_FETCH_QUILL=ON
cmake --build build-quill --target logger_quill_blocking_benchmark logger_quill_dropping_benchmark --parallel

# Linux / macOS
./build-quill/logger_quill_blocking_benchmark --threads 4 --messages 250000 --payload 128 --runs 3
./build-quill/logger_quill_dropping_benchmark --threads 4 --messages 250000 --payload 128 --runs 3

# Windows PowerShell：生产者扩展性测试。
foreach ($threads in 4, 8, 16) {
  .\build-quill\logger_quill_blocking_benchmark.exe --threads $threads --messages 250000 --payload 128 --runs 3
  .\build-quill\logger_quill_dropping_benchmark.exe --threads $threads --messages 250000 --payload 128 --runs 3
}
```

spdlog 基准独立构建，固定使用其 header-only 目标和一个异步后台线程；它不会与 Quill 的全局后端状态混用。以下是本表 spdlog 数据使用的 Windows MSVC x64 构建命令：

```bash
cmake -S . -B build-spdlog-msvc -G "Visual Studio 17 2022" -A x64 -DLOGGER_BUILD_SPDLOG_BENCHMARK=ON -DLOGGER_FETCH_SPDLOG=ON
cmake --build build-spdlog-msvc --config Release --target logger_spdlog_benchmark --parallel

# Windows PowerShell：仅运行与表格语义对齐的两个 Profile。
foreach ($threads in 4, 8, 16) {
  .\build-spdlog-msvc\Release\logger_spdlog_benchmark.exe --threads $threads --messages 250000 --payload 128 --runs 3 --profile reliable_periodic
  .\build-spdlog-msvc\Release\logger_spdlog_benchmark.exe --threads $threads --messages 250000 --payload 128 --runs 3 --profile discard_new
}
```

Quill 要求同一线程只使用一种 `FrontendOptions`，所以可靠与丢新 Profile 分别构建为两个可执行程序。spdlog v1.17.0 的 `Block` 与 `DiscardNew` 可分别对齐这两个场景；其 `OverrunOldest` 为保留最新日志的策略，但没有在本轮重新测得的 `cpp_logger::DropOldest` 对照，故不放入横向性能表。`DropOldest` 仍由本项目的单元测试覆盖。

### 本机 Quill 与 spdlog 对照结果

`cpp_logger` 与 Quill 行保留 2026-07-26 在 Windows、MSYS2 MinGW-w64 GCC 15.2.0、Release 下的结果；spdlog v1.17.0 行于 2026-07-27 在 Windows、Visual Studio 2022 x64、MSVC 19.40.33811、Release 下独立重测。每个配置均为 3 轮，每个生产者写入 250,000 条、正文 128 B，并使用本地时间毫秒格式的文本文件输出和 1 秒周期刷新；吞吐量由三轮平均耗时计算。由于编译器不同，表中 spdlog 行只记录该具体配置，不能据此做严格的跨库优劣结论。

各实现均以单后台线程完成格式化和文件 I/O。`cpp_logger` 的队列容量为 `2048 × 生产者数` 条记录，默认批量大小为 256；spdlog 使用容量同为 `2048 × 生产者数` 条记录的全局 MPMC 异步队列；Quill 的可靠模式使用 `UnboundedBlocking`（每生产者初始 256 KiB、最大 64 MiB），丢新模式使用每生产者 256 KiB 的 `BoundedDropping`。容量单位和队列拓扑并不完全相同，因此这是一项**语义尽量对齐**的端到端实验，而不是容量完全相等的微基准。为避免 Windows 将 Quill 默认 500 ns 空闲休眠放大为毫秒级，Quill 后台线程采用忙等、空闲时 `yield` 的配置；这会提高 CPU 占用，结果不应外推到低 CPU 功耗场景。

`cpp_logger` 的丢弃数来自 `droppedCount()`；Quill 的丢弃数由其入队接口返回值逐条统计；spdlog 的丢弃数来自异步线程池的 `discard_counter()`。生产者吞吐量统计全部尝试提交，端到端吞吐量只统计最终保留、完成格式化并 flush 到文件的记录。

#### 4 个生产者

| 场景 / 实现 | 队列策略 | 刷新策略 | 生产者吞吐量（万条/秒） | 端到端吞吐量（万条/秒） | 丢弃率 |
| --- | --- | --- | ---: | ---: | ---: |
| 可靠 / 周期刷新（cpp_logger） | Block | Periodic 1 s | 124.92 | 123.80 | 0.0000% |
| 可靠 / 周期刷新（spdlog v1.17.0，MSVC） | Block | Periodic 1 s | 48.30 | 48.16 | 0.0000% |
| 可靠 / 周期刷新（Quill） | UnboundedBlocking | Periodic 1 s | 874.92 | 80.53 | 0.0000% |
| 容忍丢失 / 丢新（cpp_logger） | DropNewest | Periodic 1 s | 309.61 | 120.15 | 60.3628% |
| 容忍丢失 / 丢新（spdlog v1.17.0，MSVC） | DiscardNew | Periodic 1 s | 460.29 | 76.98 | 82.7957% |
| 容忍丢失 / 丢新（Quill） | BoundedDropping | Periodic 1 s | 5273.86 | 80.30 | 97.4055% |

#### 8 个生产者

| 场景 / 实现 | 队列策略 | 刷新策略 | 生产者吞吐量（万条/秒） | 端到端吞吐量（万条/秒） | 丢弃率 |
| --- | --- | --- | ---: | ---: | ---: |
| 可靠 / 周期刷新（cpp_logger） | Block | Periodic 1 s | 103.37 | 102.60 | 0.0000% |
| 可靠 / 周期刷新（spdlog v1.17.0，MSVC） | Block | Periodic 1 s | 33.91 | 33.83 | 0.0000% |
| 可靠 / 周期刷新（Quill） | UnboundedBlocking | Periodic 1 s | 943.82 | 73.11 | 0.0000% |
| 容忍丢失 / 丢新（cpp_logger） | DropNewest | Periodic 1 s | 230.12 | 103.53 | 54.2425% |
| 容忍丢失 / 丢新（spdlog v1.17.0，MSVC） | DiscardNew | Periodic 1 s | 428.28 | 47.40 | 88.6532% |
| 容忍丢失 / 丢新（Quill） | BoundedDropping | Periodic 1 s | 5906.90 | 65.62 | 97.8928% |

#### 16 个生产者

| 场景 / 实现 | 队列策略 | 刷新策略 | 生产者吞吐量（万条/秒） | 端到端吞吐量（万条/秒） | 丢弃率 |
| --- | --- | --- | ---: | ---: | ---: |
| 可靠 / 周期刷新（cpp_logger） | Block | Periodic 1 s | 72.30 | 71.62 | 0.0000% |
| 可靠 / 周期刷新（spdlog v1.17.0，MSVC） | Block | Periodic 1 s | 33.77 | 33.70 | 0.0000% |
| 可靠 / 周期刷新（Quill） | UnboundedBlocking | Periodic 1 s | 843.36 | 68.28 | 0.0000% |
| 容忍丢失 / 丢新（cpp_logger） | DropNewest | Periodic 1 s | 123.35 | 36.84 | 69.7742% |
| 容忍丢失 / 丢新（spdlog v1.17.0，MSVC） | DiscardNew | Periodic 1 s | 463.51 | 26.23 | 94.1834% |
| 容忍丢失 / 丢新（Quill） | BoundedDropping | Periodic 1 s | 6194.27 | 46.62 | 98.3951% |

结果分析：

- 三个可靠 Profile 均无丢失。spdlog 的 MSVC 数据使用全局 MPMC 队列和单工作线程；cpp_logger、Quill 行则来自 MinGW。编译器、标准库与文件 I/O 实现均会影响结果，因此它们不能作为严格的跨库吞吐排名。
- 丢新模式下，Quill 与 spdlog 的提交速率很高，但分别有约 82.8%–97% 以上记录被拒绝；这些数字不是可靠写入能力。端到端吞吐应结合最终保留量和业务可接受的丢失上限判断。
- spdlog 的异步队列、Quill 的每生产者 SPSC 队列、各自的格式化器和文件 Sink 都与本项目不同；这组数据只描述上述具体配置，不是跨平台、跨编译器、跨存储介质的通用库排名。
- `DropOldest` 继续由本项目的单元测试覆盖。spdlog 的 `OverrunOldest` 虽属相近语义，但由于本轮没有重跑对应的 `cpp_logger::DropOldest` 数据，未放入表中，避免用不同时次的数据做横向结论。

CPU、存储介质、文件系统缓存、编译器和系统负载都会影响结果；请使用上述命令在同一台机器上复测。

## 🗺️ 后续计划

- [x] 支持控制台与可插拔输出端
- [ ] 支持日志文件保留期与自动清理
- [ ] 补充更多持续集成检查（格式化、静态分析）

## 🤝 贡献

欢迎提交 Issue 和 Pull Request。提交前请至少运行：

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 📄 许可证

本项目采用 [MIT License](LICENSE)。
