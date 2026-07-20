<div align="center">

# C++ Cross-Platform Asynchronous Logging System

[English](README_EN.md) | [简体中文](README.md)

A lightweight cross-platform multithreaded asynchronous logging system implemented in C++17.

Application threads capture raw log records and push them into `BlockingQueue`; a background thread formats and writes batches to disk, shortening the logging path in application threads.

![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.14%2B-brightgreen.svg)
![GitHub top language](https://img.shields.io/github/languages/top/qxf-72/cpp_logger)
![Platform](https://img.shields.io/badge/Platform-Cross%20platform-brightgreen.svg)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

⭐ If this project helps you, a Star is appreciated!

</div>

## ✨ Features

- Supports five log levels: `DEBUG / INFO / WARN / ERROR / FATAL`
- Supports log level filtering to avoid unnecessary string construction
- Supports millisecond-precision timestamps
- Records thread ID, source file name, and line number
- Provides convenient macros such as `LOG_INFO("message")`
- Implements a producer-consumer model based on `BlockingQueue`
- Writes logs asynchronously through a background thread
- Supports automatic log file rotation by date
- Supports automatic log file rotation by file size
- Supports configurable bounded-queue capacity and full-queue policies (block, drop newest, drop oldest)
- Exposes dropped-log count, current queue size, and queue peak size
- Formats and writes batches in the background to reduce stream-output overhead
- Supports safe shutdown and drains remaining logs in the queue
- Uses CMake to build a static library, demo program, and performance benchmark

## 📁 Project Structure

```text
cpp_logger/
|-- benchmark/
|   `-- benchmark.cpp
|-- examples/
|   `-- example.cpp
|-- tests/
|   `-- logger_tests.cpp
|-- include/
|   |-- BlockingQueue.h
|   `-- Logger.h
|-- src/
|   `-- Logger.cpp
|-- CMakeLists.txt
|-- LICENSE
|-- README.md
`-- README_EN.md
```

## 🚀 Build and Run

### Requirements

- Windows, Linux, or macOS
- C++17 or later
- CMake 3.14 or later
- GCC, Clang, or MSVC

### Build

```bash
git clone https://github.com/qxf-72/cpp_logger.git
cd cpp_logger

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Run Unit Tests

The project integrates automated unit tests with CTest. The default configuration builds `logger_tests`; run it after building:

```bash
# Single-config generators such as Ninja and Unix Makefiles
ctest --test-dir build --output-on-failure

# Multi-config generators such as Visual Studio
ctest --test-dir build -C Release --output-on-failure
```

The tests cover the blocking queue lifecycle and full-queue policies, initialization validation, log-level filtering, queue draining in `stop()`, size-based rotation, reinitialization, and concurrent logging.

After the build completes, the following CMake targets are generated:

```text
build/
|-- logger (static library; file extension depends on the toolchain)
|-- logger_demo
|-- logger_benchmark
`-- logger_tests
```

### Run the Demo

For single-config generators such as Ninja or Unix Makefiles:

```bash
./build/logger_demo
```

For multi-config generators such as Visual Studio:

```bash
./build/Release/logger_demo
```

View the generated log files:

```bash
ls logs/app_*.log
tail -n 20 logs/app_*.log
```

## 📖 Usage Example

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
  config.overflowPolicy = OverflowPolicy::DropNewest;
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
  LOG_FATAL("fatal message");

  std::cout << "dropped=" << Logger::instance().droppedCount() << '\n';
  Logger::instance().stop();
  return 0;
}
```

The original `init("app", LogLevel::DEBUG, 10 * 1024 * 1024)` overload remains available. It uses the default capacity of `8192`, the `Block` policy, 256-record batches, and periodic flushing.

## 🧱 Bounded Queue and Full-Queue Policies

`LoggerConfig::queueCapacity` must be greater than `0`; its default is `8192`. When the queue is full, `overflowPolicy` controls the behavior:

| Policy | Behavior | Suitable when |
| --- | --- | --- |
| `OverflowPolicy::Block` | Blocks producers until the background thread consumes a message | Logs must not be lost |
| `OverflowPolicy::DropNewest` | Drops the new message and returns immediately | Throughput and application latency take priority |
| `OverflowPolicy::DropOldest` | Drops the oldest queued message and retains the new one | Recent state matters most during troubleshooting |

`droppedCount()` counts losses caused by either dropping policy; `queueSize()` returns the pending-message count; `queuePeakSize()` returns the peak queue size for the current initialization. A successful `init()` resets all three statistics.

## 💾 Flush and Batch Policies

`writeBatchSize` controls the maximum records formatted and written together; it defaults to `256`. `flushPolicy` controls when the C++ stream is flushed to the operating-system buffer:

| Policy | Behavior | Suitable when |
| --- | --- | --- |
| `FlushPolicy::OnStop` | Flushes on `stop()` only, except for `flushAtOrAbove` records | Maximum throughput; explicit graceful shutdown is guaranteed |
| `FlushPolicy::Periodic` | Flushes at `flushInterval` (1 second by default), and for `flushAtOrAbove` records | Default balance between throughput and observability |
| `FlushPolicy::EveryBatch` | Flushes after every writer batch | Lower visibility latency at higher overhead |

`flushAtOrAbove` defaults to `LogLevel::ERROR`; set it to `std::nullopt` to disable level-triggered flushing. A level-triggered flush occurs after the background thread processes the record, so it does not make the producer call synchronous. `std::ofstream::flush()` is not `fsync` and does not provide power-loss durability.

## 📝 Log Format

```text
[2026-06-25 12:00:00.123][INFO][tid:140123456789000][../examples/example.cpp:42] message
```

Format description:

```text
[time][log level][thread ID][source file:line] log message
```

## 🗂️ Log Rotation

Log file name format:

```text
file_prefix_date_index.log
```

Examples:

```text
app_2026-06-25_0.log
app_2026-06-25_1.log
app_2026-06-26_0.log
```

Rotation rules:

- When the date changes, a new `0` log file is created for the new date
- When the current log file reaches the size limit, the file index is incremented
- Multiple log files can be generated on the same day

## 🏗️ Core Design

```text
application thread
  `-- capture timestamp, level, thread ID, source location, and message
        `-- push into BlockingQueue
              `-- return or wait for capacity according to the overflow policy

background logging thread
  `-- pop a batch from BlockingQueue
        `-- format records and check whether rotation is needed
              `-- write the batch to the log file
```

`BlockingQueue` uses:

- `std::mutex` to protect the shared queue
- `std::condition_variable` to block and wake the consumer
- `close()` to wake the background thread and exit safely

After `Logger::stop()` is called, the queue no longer accepts new logs. The background thread processes all existing logs, then flushes and closes the log file.

## 🛣️ Roadmap

- [x] Add performance tests for asynchronous logging
- [x] Support a bounded queue and full-queue policies
- [ ] Support console output
- [x] Add unit tests
- [ ] Support automatic cleanup of log files
- [ ] Support installation and CMake package export

## 🤝 Contributing

Issues and Pull Requests are welcome.

Before submitting code, make sure the following command completes successfully:

```bash
cmake --build build
```

Please also keep the code style consistent.

## 📊 Benchmark

After building, run `logger_benchmark` to measure both producer submission throughput and end-to-end throughput after all logs are persisted.

```bash
# Single-config generators such as Ninja or Unix Makefiles
./build/logger_benchmark --threads 4 --messages 50000 --payload 128 --runs 5

# Multi-config generators such as Visual Studio
./build/Release/logger_benchmark --threads 4 --messages 50000 --payload 128 --runs 5
```

Available options:

```text
--threads <N>    Producer thread count
--messages <N>   Messages written by each producer
--payload <N>    Payload size of each message in bytes
--runs <N>       Number of repeated runs
--output <PATH>  Directory for temporary log files
--keep-logs      Keep log files generated by each run
```

The output is CSV. `producer_logs_per_second` measures submission by producer threads only, while `end_to_end_logs_per_second` also includes `stop()` draining the queue and flushing the log file. `--batch-size`, `--flush-policy`, and `--flush-interval-ms` expose the writer configuration. Temporary logs are removed after each run unless `--keep-logs` is specified.

### cpp_logger vs spdlog

The comparison target is opt-in, so normal builds do not download dependencies. It first looks for an installed spdlog package; append `-DLOGGER_FETCH_SPDLOG=ON` to fetch the pinned v1.17.0 dependency into the build directory when none is found.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLOGGER_BUILD_SPDLOG_BENCHMARK=ON -DLOGGER_FETCH_SPDLOG=ON
cmake --build build --config Release

./build/logger_spdlog_benchmark --threads 4 --messages 250000 --payload 128 --runs 3
```

The runner prints a Markdown table for four predefined modes. Both Block modes use one asynchronous writer thread, the same 8192-record queue capacity, the same message payload and source-location pattern, and a periodic 1-second flush policy. `spdlog reference` is therefore a control for the balanced mode. The producer rate counts attempts; the end-to-end rate counts accepted records, so the DropNewest profile does not hide loss behind a high submission rate.

| Mode | Queue policy | Flush policy | Producer throughput | End-to-end throughput | Drop rate |
| --- | --- | --- | ---: | ---: | ---: |
| Reliable (`cpp_logger`) | Block | EveryBatch | 686,509 logs/s | 679,626 logs/s | 0.0000% |
| Balanced (`cpp_logger`) | Block | Periodic 1 s | 675,741 logs/s | 669,458 logs/s | 0.0000% |
| Producer-low-latency (`cpp_logger`) | DropNewest | Periodic 1 s | 6,126,948 logs/s | 778,154 logs/s | 86.4329% |
| spdlog reference (v1.17.0) | Block | Periodic 1 s | 581,949 logs/s | 579,614 logs/s | 0.0000% |

The table above was measured in the same local Release build with four producer threads, 250,000 messages per thread, a 128-byte payload, and three runs per mode. Results depend on CPU, storage, filesystem caching, compiler, and system load; use the command above to compare revisions on the same machine.

`std::ofstream::flush()` flushes the C++ stream and operating-system buffers, but is not equivalent to physical-disk `fsync`. The end-to-end figure therefore measures queue draining and flushing by the logger, not power-loss-safe persistence latency.

## 📄 License

This project is licensed under the MIT License.
