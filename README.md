# C++ Linux 异步日志系统

![GitHub License](https://img.shields.io/github/license/qxf-72/cpp_logger)
![GitHub top language](https://img.shields.io/github/languages/top/qxf-72/cpp_logger)


## 项目简介

本项目是一个基于 C++11 实现的 Linux 多线程异步日志系统，支持日志级别过滤、时间戳、线程 ID、源码文件名与行号输出。

项目使用 `BlockingQueue` 和后台日志线程实现异步写入：业务线程只负责生成日志并写入阻塞队列，文件 IO 由后台线程完成，从而减少日志写入对业务线程的阻塞。

## 当前功能

- 支持 DEBUG / INFO / WARN / ERROR 四种日志级别
- 支持日志级别过滤
- 支持输出时间戳、线程 ID、源码文件名和行号
- 支持宏调用方式，例如 `LOG_INFO("message")`
- 基于 `BlockingQueue` 实现生产者-消费者模型
- 使用后台线程异步写入日志文件
- 支持程序退出时安全停止后台线程并刷新剩余日志
- 使用 CMake 构建项目

## 项目结构

```text
cpp_logger/
├── CMakeLists.txt
├── include/
│   ├── BlockingQueue.h
│   └── Logger.h
└── src/
    ├── Logger.cpp
    └── main.cpp
```

## 编译运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build
./logger_demo
```

程序运行后会在 `build/` 目录下生成日志文件：

```text
app.log
```

查看日志：

```bash
tail -n 20 app.log
```

## 使用示例

```cpp
#include "Logger.h"

int main() {
    Logger::instance().init("app.log", LogLevel::DEBUG);

    LOG_INFO("program started");
    LOG_WARN("this is a warning");
    LOG_ERROR("something wrong");

    Logger::instance().stop();

    return 0;
}
```

## 日志格式

```text
[2026-05-25 12:00:00.123][INFO][tid:140123456789000][../src/main.cpp:18] message
```

字段含义：

```text
[时间][日志级别][线程ID][源码文件:行号] 日志内容
```

## 核心设计

同步日志中，业务线程需要直接写文件：

```text
业务线程 -> 加锁 -> 写文件
```

本项目采用异步日志模型：

```text
业务线程 -> 生成日志 -> 写入 BlockingQueue -> 立即返回
后台线程 -> 从 BlockingQueue 取日志 -> 写入文件
```

`BlockingQueue` 使用 `std::mutex` 保证队列线程安全，使用 `std::condition_variable` 在队列为空时阻塞后台线程，避免忙等。

程序退出时，`Logger::stop()` 会关闭队列、唤醒后台线程、等待剩余日志写入完成，并最终关闭日志文件。

## 后续计划

- 支持日志文件按大小滚动
- 支持按日期切分日志文件
- 添加 benchmark 性能测试
- 支持最大队列长度限制
- 支持控制台输出
- 封装为可复用日志库

## 开发环境

- Linux
- C++11
- CMake
- pthread

---
