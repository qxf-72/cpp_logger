<div align="center">

# C++ Linux Asynchronous Logging System

[English](README_EN.md) | [简体中文](README.md)

A lightweight multithreaded asynchronous logging system for Linux, implemented in C++17.

Application threads format log messages and push them into `BlockingQueue`, while a background thread writes logs to disk. This reduces file I/O blocking in application threads.

![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.10%2B-brightgreen.svg)
![GitHub top language](https://img.shields.io/github/languages/top/qxf-72/cpp_logger)
![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)
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
- Supports safe shutdown and drains remaining logs in the queue
- Uses CMake to build a static library and demo program

## 📁 Project Structure

```text
cpp_logger/
|-- include/
|   |-- BlockingQueue.h
|   `-- Logger.h
|-- src/
|   `-- Logger.cpp
|-- test/
|   `-- test.cpp
|-- CMakeLists.txt
|-- LICENSE
`-- README.md
```

## 🚀 Build and Run

### Requirements

- Linux
- C++11 or later
- CMake 3.10 or later
- GCC or Clang

### Build

```bash
git clone https://github.com/qxf-72/cpp_logger.git
cd cpp_logger

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

After the build completes, the following files are generated:

```text
build/
|-- liblogger.a
`-- logger_demo
```

### Run the Demo

```bash
cd build
./logger_demo
```

View the generated log files:

```bash
ls app_*.log
tail -n 20 app_*.log
```

## 📖 Usage Example

```cpp
#include "Logger.h"

int main() {
  // Parameters:
  // log file prefix, minimum log level, single-file size limit
  if (!Logger::instance().init(
          "app", LogLevel::DEBUG, 10 * 1024 * 1024)) {
    return 1;
  }

  LOG_DEBUG("debug message");
  LOG_INFO("program started");
  LOG_WARN("warning message");
  LOG_ERROR("error message");
  LOG_FATAL("fatal message");

  Logger::instance().stop();
  return 0;
}
```

## 📝 Log Format

```text
[2026-06-25 12:00:00.123][INFO][tid:140123456789000][../test/test.cpp:42] message
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
  `-- format log message
        `-- push into BlockingQueue
              `-- return immediately

background logging thread
  `-- pop from BlockingQueue
        `-- check whether rotation is needed
              `-- write to log file
```

`BlockingQueue` uses:

- `std::mutex` to protect the shared queue
- `std::condition_variable` to block and wake the consumer
- `close()` to wake the background thread and exit safely

After `Logger::stop()` is called, the queue no longer accepts new logs. The background thread processes all existing logs, then flushes and closes the log file.

## 🛣️ Roadmap

- [ ] Add performance tests for synchronous and asynchronous logging
- [ ] Support a capacity limit for the blocking queue
- [ ] Support console output
- [ ] Add unit tests
- [ ] Support automatic cleanup of log files
- [ ] Support installation and CMake package export

## 🤝 Contributing

Issues and Pull Requests are welcome.

Before submitting code, make sure the following command completes successfully:

```bash
cmake --build build
```

Please also keep the code style consistent.

## 📄 License

This project is licensed under the MIT License.
