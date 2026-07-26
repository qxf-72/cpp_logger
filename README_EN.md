<div align="center">

# cpp_logger

**A lightweight asynchronous logging library for C++17**

English | [简体中文](README.md)

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B)
![CMake](https://img.shields.io/badge/CMake-3.14%2B-064F8C?logo=cmake)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-555555?logo=linux&logoColor=FCC624)
[![CI](https://github.com/qxf-72/cpp_logger/actions/workflows/ci.yml/badge.svg)](https://github.com/qxf-72/cpp_logger/actions/workflows/ci.yml)
![Release](https://img.shields.io/github/v/release/qxf-72/cpp_logger?display_name=tag&logo=github)
[![License](https://img.shields.io/github/license/qxf-72/cpp_logger?color=yellow)](LICENSE)
![Stars](https://img.shields.io/github/stars/qxf-72/cpp_logger?logo=github&label=stars&color=F5C518)

</div>

`cpp_logger` queues log records in application threads, then lets a background thread format and write them to files in batches. It includes configurable overload policies, file rotation, flush policies, runtime statistics, CTest coverage, and an installable CMake package.

## ✨ Highlights

| Capability | Details |
| --- | --- |
| Asynchronous writing | Application threads submit `LogRecord` objects; the background thread formats, rotates, and dispatches them to sinks. |
| Bounded queue | `Block`, `DropNewest`, and `DropOldest` policies are available when the queue is full. |
| Batch processing | Records are popped, formatted, and written in batches to reduce synchronization and stream overhead. |
| Observability | `droppedCount()`, `queueSize()`, and `queuePeakSize()` expose queue state. |
| Log management | Five levels, millisecond timestamps, thread IDs, source locations, and date/size rotation. |
| Shutdown and flushing | On-stop, periodic, and per-batch flushing; `stop()` drains accepted records. |
| Multiple sinks | Built-in rotating `FileSink` and `ConsoleSink`, plus custom `LogSink` support. |
| Tooling | CTest, cross-platform GitHub Actions CI, and `find_package` package export. |

## 🚀 Quick Start

### Requirements

- A C++17 compiler: GCC, Clang, or MSVC
- CMake 3.14 or later
- Ninja is recommended; another locally available CMake generator also works

The commands below use Ninja:

```bash
git clone https://github.com/qxf-72/cpp_logger.git
cd cpp_logger

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the demo:

```bash
# Linux / macOS
./build/logger_demo

# Windows PowerShell
.\build\logger_demo.exe
```

Logs are written to `logs/` by default.

## 🧩 Usage

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

`LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, and `LOG_FATAL` check the configured level before evaluating the message expression, avoiding unnecessary string construction for filtered logs.

The original `init("app", LogLevel::DEBUG, 10 * 1024 * 1024)` overload remains available. It uses the default capacity of `8192`, the `Block` policy, 256-record batches, and periodic flushing.

### Full-Queue Policies

| Policy | Behavior | Suitable when |
| --- | --- | --- |
| `OverflowPolicy::Block` | Blocks producers until the background thread consumes a record or the logger closes. | Logs cannot be lost. |
| `OverflowPolicy::DropNewest` | Drops the incoming record and returns immediately. | Application latency takes priority. |
| `OverflowPolicy::DropOldest` | Removes the oldest queued record and retains the incoming one. | The latest state is most useful for diagnosis. |

`droppedCount()` counts losses caused by either drop policy. `queueSize()` is the pending-record count, while `queuePeakSize()` is the peak size for the current initialization. A successful `init()` resets all three statistics.

### Batching and Flushing

| Setting | Meaning |
| --- | --- |
| `writeBatchSize` | Maximum records formatted and written together; defaults to `256`. |
| `FlushPolicy::OnStop` | Flushes only in `stop()`, except records at `flushAtOrAbove`. |
| `FlushPolicy::Periodic` | Flushes at `flushInterval`, one second by default; this is the default policy. |
| `FlushPolicy::EveryBatch` | Flushes after every writer batch, improving visibility at a higher cost. |

`flushAtOrAbove` defaults to `LogLevel::ERROR`; set it to `std::nullopt` to disable level-triggered flushes. The `std::ofstream::flush()` used by `FileSink` flushes the C++ stream to the operating system, but is not `fsync` and does not provide power-loss durability.

### Output Sinks

`FileSink` is enabled by default and handles date/size rotation. `ConsoleSink` is optional and writes to standard output or standard error. Both built-in sinks can be enabled together and receive the same formatted batch:

```cpp
config.enableFileSink = true;               // Default; basePath is required when enabled.
config.enableConsoleSink = true;
config.consoleStream = ConsoleStream::Stderr;
```

When the file sink is disabled, `basePath` is no longer required and the logger can use only the console or custom sinks. Derive a custom sink from `LogSink` and add it to `additionalSinks`; its `writeBatch()`, `flush()`, and `close()` methods are all called by the background logger thread.

## 🏗️ Core Data Flow

```mermaid
flowchart LR
    P[Application thread] --> R[Capture LogRecord]
    R --> Q[Bounded BlockingQueue]
    Q --> W[Background logger thread]
    W --> F[Format a batch]
    F --> S[FileSink / ConsoleSink / custom sink]
    S --> D[Write a batch]
```

- Application threads capture the timestamp, level, thread ID, source location, and message, then enqueue the record; string formatting is deferred to the background thread.
- `close()` wakes waiting threads. `stop()` rejects later writes, drains accepted records, then flushes and closes the file.
- Lifecycle synchronization and queue generations prevent producers from an old lifecycle writing into a reinitialized queue.

The log format is:

```text
[2026-06-25 12:00:00.123][INFO][tid:140123456789000][example.cpp:42] message
```

Log files use `<prefix>_<date>_<index>.log`, for example `app_2026-06-25_0.log`. A new file is created when the date changes or the file reaches `maxFileSize`.

## ✅ Tests and CI

On every push to `main` and every pull request, GitHub Actions configures, builds, runs CTest, and validates CMake installation on Windows, Linux, and macOS.

Run tests locally:

```bash
ctest --test-dir build --output-on-failure
```

Tests cover queue lifecycle and overload policies, configuration validation, level filtering, safe draining during `stop()`, size rotation, reinitialization, and concurrent logging.

## 📦 Install and Use as a CMake Package

Install the static library, headers, and CMake package after building:

```bash
cmake --install build --prefix <install-prefix>
```

Consume it from another CMake project:

```cmake
find_package(cpp_logger 0.1 CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE cpp_logger::logger)
```

Pass the installation prefix when configuring the consumer:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<install-prefix>
```

### Use a Prebuilt Package from GitHub Releases

Starting with `v0.1.1`, pushing a `vX.Y.Z` tag that matches the CMake project version automatically builds and publishes prebuilt CMake packages. Download and extract the asset matching your platform, compiler, and architecture; cloning this repository is not required:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<extracted cpp_logger directory>
cmake --build build --parallel
```

Packages are provided for Windows MSVC x64, Linux GCC x64, and macOS AppleClang arm64. Each Release includes `SHA256SUMS.txt` for verification. Prebuilt static libraries must not be mixed across platforms or compiler toolchains.

## 📊 Benchmark

The regular benchmark target is enabled by default and produces CSV output. It reports both producer submission throughput and end-to-end throughput after `stop()` drains and flushes the queue.

```bash
# Linux / macOS
./build/logger_benchmark --threads 4 --messages 50000 --payload 128 --runs 5

# Windows PowerShell
.\build\logger_benchmark.exe --threads 4 --messages 50000 --payload 128 --runs 5
```

| Option | Description |
| --- | --- |
| `--threads <N>` | Producer count; default: 4. |
| `--messages <N>` | Records per producer; default: 50000. |
| `--payload <N>` | Message body size in bytes; default: 128. |
| `--runs <N>` | Repeated runs; default: 3. |
| `--batch-size <N>` | Writer batch size; default: 256. |
| `--flush-policy <...>` | `on-stop`, `periodic`, or `every-batch`. |

An optional comparison target pins `spdlog` to v1.17.0. It is off by default, so regular builds do not download dependencies. When `spdlog` is unavailable locally, CMake can fetch it into the build tree:

```bash
cmake -S . -B build-spdlog -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_SPDLOG_BENCHMARK=ON -DLOGGER_FETCH_SPDLOG=ON
cmake --build build-spdlog --parallel

# Linux / macOS
./build-spdlog/logger_spdlog_benchmark --threads 4 --messages 250000 --payload 128 --runs 3

# Windows PowerShell
.\build-spdlog\logger_spdlog_benchmark.exe --threads 4 --messages 250000 --payload 128 --runs 3
```

One local Release measurement, using four producers, 250,000 records per producer, a 128-byte payload, and three runs per profile, produced the following results. These figures are for same-machine revision comparison only, not a general performance claim.

| Mode | Queue policy | Flush policy | Producer rate | End-to-end rate | Drop rate |
| --- | --- | --- | ---: | ---: | ---: |
| Reliable (`cpp_logger`) | Block | EveryBatch | 686,509 logs/s | 679,626 logs/s | 0.0000% |
| Balanced (`cpp_logger`) | Block | Periodic 1 s | 675,741 logs/s | 669,458 logs/s | 0.0000% |
| Producer-low-latency (`cpp_logger`) | DropNewest | Periodic 1 s | 6,126,948 logs/s | 778,154 logs/s | 86.4329% |
| spdlog reference (v1.17.0) | Block | Periodic 1 s | 581,949 logs/s | 579,614 logs/s | 0.0000% |

The compared modes use one asynchronous writer, an 8192-record queue, the same payload, and the same source-location pattern. CPU, storage, filesystem cache, compiler, and system load all affect results; rerun the command on the same machine when comparing revisions.

## 🗺️ Roadmap

- [x] Console and pluggable output sinks
- [ ] Log-retention and automatic-cleanup policies
- [ ] More CI checks, including formatting and static analysis

## 🤝 Contributing

Issues and pull requests are welcome. Before submitting code, run:

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 📄 License

Licensed under the [MIT License](LICENSE).
