<div align="center">

# cpp_logger

**基于 C++17 的轻量级异步日志库**

[English](README_EN.md) | 简体中文

[![CI](https://github.com/qxf-72/cpp_logger/actions/workflows/ci.yml/badge.svg)](https://github.com/qxf-72/cpp_logger/actions/workflows/ci.yml)
![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.14%2B-brightgreen.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-brightgreen.svg)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

</div>

`cpp_logger` 将日志记录先写入有界队列，再由后台线程批量格式化、批量写入文件。它提供可选的队列过载策略、文件滚动、刷新策略、运行统计、CTest 测试和 CMake 包导出，适合作为 C++ 并发与异步 I/O 的学习型项目，也可作为轻量级文件日志组件使用。

## ✨ 特性

| 能力 | 说明 |
| --- | --- |
| 异步写入 | 业务线程提交 `LogRecord`；后台线程负责格式化、滚动和文件 I/O。 |
| 有界队列 | 支持 `Block`、`DropNewest`、`DropOldest` 三种队列满策略。 |
| 批量处理 | 按批出队、格式化和写入，减少同步与流写入次数。 |
| 可观测性 | 提供 `droppedCount()`、`queueSize()` 与 `queuePeakSize()`。 |
| 日志管理 | 五级过滤、毫秒时间戳、线程 ID、源文件位置、按日期/大小滚动。 |
| 刷新与停止 | 支持停止时、定时、每批刷新；`stop()` 会排空已接收的日志。 |
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

`flushAtOrAbove` 默认是 `LogLevel::ERROR`；设置为 `std::nullopt` 可关闭按级别刷新。这里的 `std::ofstream::flush()` 将 C++ 流缓冲刷新到操作系统，并不等同于 `fsync`，因此不提供断电安全的持久化保证。

## 🏗️ 核心数据流

```mermaid
flowchart LR
    P[业务线程] --> R[采集 LogRecord]
    R --> Q[有界 BlockingQueue]
    Q --> W[后台日志线程]
    W --> F[批量格式化]
    F --> O[按日期/大小滚动]
    O --> D[批量写入日志文件]
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

可选的 `spdlog` 对照压测固定使用 v1.17.0；默认关闭，不会影响常规构建。如本机没有 `spdlog`，可由 CMake 在构建目录下载：

```bash
cmake -S . -B build-spdlog -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_SPDLOG_BENCHMARK=ON -DLOGGER_FETCH_SPDLOG=ON
cmake --build build-spdlog --parallel

# Linux / macOS
./build-spdlog/logger_spdlog_benchmark --threads 4 --messages 250000 --payload 128 --runs 3

# Windows PowerShell
.\build-spdlog\logger_spdlog_benchmark.exe --threads 4 --messages 250000 --payload 128 --runs 3
```

一次本机 Release 测试（4 个生产者、每线程 250,000 条、128 B 正文、每种模式 3 轮）的结果如下。它只用于同机版本比较，不代表跨设备或通用场景的结论。

| 模式 | 队列策略 | 刷新策略 | 生产者吞吐量 | 端到端吞吐量 | 丢弃率 |
| --- | --- | --- | ---: | ---: | ---: |
| 可靠模式（cpp_logger） | Block | EveryBatch | 686,509 条/秒 | 679,626 条/秒 | 0.0000% |
| 平衡模式（cpp_logger） | Block | Periodic 1 s | 675,741 条/秒 | 669,458 条/秒 | 0.0000% |
| 低延迟模式（cpp_logger） | DropNewest | Periodic 1 s | 6,126,948 条/秒 | 778,154 条/秒 | 86.4329% |
| spdlog 对照（v1.17.0） | Block | Periodic 1 s | 581,949 条/秒 | 579,614 条/秒 | 0.0000% |

对照模式使用一个异步写线程、8192 条队列容量、相同正文与源位置格式。结果会受到 CPU、存储介质、文件系统缓存、编译器和系统负载影响；请使用上述命令在同一台机器上复测。

## 🗺️ 后续计划

- [ ] 支持控制台与可插拔输出端
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
