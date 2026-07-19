<div align="center">

# C++ 跨平台异步日志系统

[English](README_EN.md) | [简体中文](README.md)

一个基于 C++17 实现的轻量级跨平台多线程异步日志系统。

业务线程只采集原始日志记录并写入 `BlockingQueue`；后台线程批量格式化、批量落盘，从而缩短业务线程的日志调用路径。

![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.10%2B-brightgreen.svg)
![GitHub top language](https://img.shields.io/github/languages/top/qxf-72/cpp_logger)
![Platform](https://img.shields.io/badge/Platform-Cross%20platform-brightgreen.svg)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

⭐ 如果这个项目对你有帮助，欢迎点一个 Star！

</div>

## ✨ 功能特性

- 支持 `DEBUG / INFO / WARN / ERROR / FATAL` 五种日志级别
- 支持日志级别过滤，避免无效日志的字符串构造
- 支持毫秒级时间戳
- 支持记录线程 ID、源码文件名和行号
- 支持 `LOG_INFO("message")` 等宏调用方式
- 基于 `BlockingQueue` 实现生产者—消费者模型
- 使用后台线程异步写入日志
- 支持按日期自动滚动日志文件
- 支持按文件大小自动滚动日志文件
- 支持配置有界队列容量及满队列策略（阻塞、丢弃最新、丢弃最旧）
- 支持查看丢弃数量、当前队列长度和队列峰值
- 后台线程批量格式化并使用批量写入降低流输出开销
- 支持安全停止并写完队列中的剩余日志
- 使用 CMake 构建静态库、示例程序和性能压测工具

## 📁 项目结构

```text
cpp_logger/
├── benchmark/
│   └── benchmark.cpp
├── examples/
│   └── example.cpp
├── tests/
│   └── logger_tests.cpp
├── include/
│   ├── BlockingQueue.h
│   └── Logger.h
├── src/
│   └── Logger.cpp
├── CMakeLists.txt
├── LICENSE
├── README.md
└── README_EN.md
```

## 🚀 编译运行

### 环境要求

- Windows、Linux 或 macOS
- C++17 或更高版本
- CMake 3.10 或更高版本
- GCC、Clang 或 MSVC

### 构建项目

```bash
git clone https://github.com/qxf-72/cpp_logger.git
cd cpp_logger

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### 运行单元测试

项目使用 CTest 集成自动化单元测试；默认配置会构建 `logger_tests`。完成构建后运行：

```bash
# Ninja、Unix Makefiles 等单配置生成器
ctest --test-dir build --output-on-failure

# Visual Studio 等多配置生成器
ctest --test-dir build -C Release --output-on-failure
```

测试覆盖阻塞队列生命周期和满队列策略、初始化参数校验、日志级别过滤、`stop()` 排空队列、按大小滚动、重新初始化及多线程写入。

构建完成后会生成：

```text
build/
├── logger（静态库，具体文件扩展名取决于工具链）
├── logger_demo
├── logger_benchmark
└── logger_tests
```

### 运行示例

单配置生成器（Ninja、Unix Makefiles 等）：

```bash
./build/logger_demo
```

多配置生成器（Visual Studio 等）：

```bash
./build/Release/logger_demo
```

查看生成的日志文件：

```bash
ls logs/app_*.log
tail -n 20 logs/app_*.log
```

## 📖 使用示例

```cpp
#include <iostream>

#include "Logger.h"

int main() {
  LoggerConfig config;
  config.basePath = "logs/app";
  config.minLevel = LogLevel::DEBUG;
  config.maxFileSize = 10 * 1024 * 1024;
  config.queueCapacity = 8192;
  config.overflowPolicy = OverflowPolicy::DropNewest;

  if (!Logger::instance().init(config)) {
    return 1;
  }

  LOG_DEBUG("debug message");
  LOG_INFO("program started");
  LOG_WARN("warning message");
  LOG_ERROR("error message");
  LOG_FATAL("fatal message");

  std::cout << "dropped=" << Logger::instance().droppedCount() << '\n';
  Logger::instance().stop();
  return 0;
}
```

旧的 `init("app", LogLevel::DEBUG, 10 * 1024 * 1024)` 接口仍可用，它会使用默认容量 `8192` 和 `Block` 策略。

## 🧱 有界队列与满队列策略

`LoggerConfig::queueCapacity` 必须大于 `0`，默认值为 `8192`。当队列已满时，由 `overflowPolicy` 决定行为：

| 策略 | 行为 | 适用场景 |
| --- | --- | --- |
| `OverflowPolicy::Block` | 阻塞生产者，直到后台线程取走日志 | 不允许丢日志 |
| `OverflowPolicy::DropNewest` | 丢弃本次新日志，业务线程立即返回 | 高吞吐、优先保护业务延迟 |
| `OverflowPolicy::DropOldest` | 丢弃队列中最早的日志，保留本次新日志 | 排障时更关注最新状态 |

`droppedCount()` 会统计两种丢弃策略造成的日志丢失；`queueSize()` 返回当前待写条数；`queuePeakSize()` 返回本次初始化以来的队列峰值。每次成功 `init()` 会重置这三项统计。

## 📝 日志格式

```text
[2026-06-25 12:00:00.123][INFO][tid:140123456789000][../examples/example.cpp:42] message
```

格式说明：

```text
[时间][日志级别][线程 ID][源码文件:行号] 日志内容
```

## 🗂️ 日志滚动

日志文件名格式：

```text
文件前缀_日期_序号.log
```

示例：

```text
app_2026-06-25_0.log
app_2026-06-25_1.log
app_2026-06-26_0.log
```

滚动规则：

- 日期改变时，创建新日期的 `0` 号日志文件
- 当前日志文件达到大小上限时，递增文件序号
- 同一天内可以生成多个日志文件

## 🏗️ 核心设计

```text
业务线程
  └── 采集时间、级别、线程 ID、源位置和消息
        └── 写入 BlockingQueue
              └── 按满队列策略立即返回或等待空位

后台日志线程
  └── 批量从 BlockingQueue 取出日志
        └── 格式化并判断是否需要滚动
              └── 批量写入日志文件
```

`BlockingQueue` 使用：

- `std::mutex` 保护共享队列
- `std::condition_variable` 实现消费者阻塞等待
- `close()` 唤醒后台线程并完成安全退出

调用 `Logger::stop()` 后，队列不再接收新日志，后台线程会处理完已有日志，再刷新并关闭文件。

## 🛣️ 后续计划

- [x] 添加异步日志性能测试
- [x] 支持阻塞队列容量上限和满队列策略
- [ ] 支持控制台输出
- [x] 添加单元测试
- [ ] 支持日志文件自动清理
- [ ] 支持安装和导出 CMake package

## 🤝 Contributing

欢迎提交 Issue 和 Pull Request。

提交代码前，请确保：

```bash
cmake --build build
```

能够正常完成，并保持代码格式统一。

## 📊 性能测试

构建后可运行 `logger_benchmark`，它会分别输出业务线程的提交吞吐量和日志完全落盘后的端到端吞吐量。

```bash
# Ninja、Unix Makefiles 等单配置生成器
./build/logger_benchmark --threads 4 --messages 50000 --payload 128 --runs 3

# Visual Studio 等多配置生成器
./build/Release/logger_benchmark --threads 4 --messages 50000 --payload 128 --runs 3
```

常用参数：

```text
--threads <N>    生产者线程数
--messages <N>   每个线程写入的日志数
--payload <N>    单条日志正文大小（字节）
--runs <N>       重复测试次数
--output <PATH>  临时日志目录
--keep-logs      保留每轮生成的日志文件
```

输出为 CSV。`producer_logs_per_second` 只统计业务线程提交日志的速度；`end_to_end_logs_per_second` 还包含 `stop()` 排空队列和刷新文件的时间。默认会在每轮结束后删除临时日志，避免压测占满磁盘。

### 测试结果

以下数据来自默认命令 `./build/logger_benchmark --threads 4 --messages 50000 --payload 128 --runs 3`。每轮由 4 个生产者线程各写入 50,000 条日志，共 200,000 条；日志正文为 128 B，文件滚动被关闭。

本次压测使用旧版 `init(...)` 接口的默认配置：容量为 `8192` 的有界队列和 `Block` 策略。因此生产者吞吐量包含队列满时的背压等待，反映“不丢日志”的实际代价；只能与相同队列配置下的结果比较。

| 指标 | 平均结果 |
| --- | ---: |
| 生产者耗时 | 340.929 ms |
| 端到端耗时 | 354.980 ms |
| 生产者吞吐量 | 586,633 条/秒 |
| 端到端吞吐量 | 563,413 条/秒 |

测试环境：Windows 10 Pro 64 位（10.0.19045）、AMD Ryzen 7 5800H（8 核 16 线程）、约 13.9 GiB 可见内存、Clang 21.1.8、Ninja Release 构建。相较于引入后台格式化和批量写入前的同配置基线，生产者与端到端吞吐量分别提升约 23% 和 21%。结果会受 CPU、磁盘、文件系统缓存和系统负载影响，仅用于同一环境下的版本比较。

`std::ofstream::flush()` 会刷新 C++ 流和操作系统缓存，但不等同于物理磁盘 `fsync`；因此端到端指标表示日志库完成排空和刷新，而非断电安全的持久化延迟。

## 📄 License

本项目使用 MIT License。
