<div align="center">

# C++ Linux 异步日志系统

一个基于 C++11 实现的轻量级 Linux 多线程异步日志系统。

业务线程负责格式化日志并写入 `BlockingQueue`，后台线程负责日志落盘，从而降低文件 IO 对业务线程的阻塞。

![C++](https://img.shields.io/badge/C%2B%2B-11-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.10%2B-brightgreen.svg)
![GitHub top language](https://img.shields.io/github/languages/top/qxf-72/cpp_logger)
![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)
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
- 支持安全停止并写完队列中的剩余日志
- 使用 CMake 构建静态库和测试程序

## 📁 项目结构

```text
cpp_logger/
├── include/
│   ├── BlockingQueue.h
│   └── Logger.h
├── src/
│   └── Logger.cpp
├── test/
│   └── test.cpp
├── CMakeLists.txt
├── LICENSE
└── README.md
```

## 🚀 编译运行

### 环境要求

- Linux
- C++11 或更高版本
- CMake 3.10 或更高版本
- GCC 或 Clang

### 构建项目

```bash
git clone https://github.com/qxf-72/cpp_logger.git
cd cpp_logger

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

构建完成后会生成：

```text
build/
├── liblogger.a
└── logger_demo
```

### 运行示例

```bash
cd build
./logger_demo
```

查看生成的日志文件：

```bash
ls app_*.log
tail -n 20 app_*.log
```

## 📖 使用示例

```cpp
#include "Logger.h"

int main() {
  // 参数依次为：
  // 日志文件前缀、最低日志级别、单个文件大小上限
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

## 📝 日志格式

```text
[2026-06-25 12:00:00.123][INFO][tid:140123456789000][../test/test.cpp:42] message
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
  └── 格式化日志
        └── 写入 BlockingQueue
              └── 立即返回

后台日志线程
  └── 从 BlockingQueue 取出日志
        └── 判断是否需要滚动
              └── 写入日志文件
```

`BlockingQueue` 使用：

- `std::mutex` 保护共享队列
- `std::condition_variable` 实现消费者阻塞等待
- `close()` 唤醒后台线程并完成安全退出

调用 `Logger::stop()` 后，队列不再接收新日志，后台线程会处理完已有日志，再刷新并关闭文件。

## 🛣️ 后续计划

- [ ] 添加同步与异步日志性能测试
- [ ] 支持阻塞队列容量上限
- [ ] 支持控制台输出
- [ ] 添加单元测试
- [ ] 支持日志文件自动清理
- [ ] 支持安装和导出 CMake package

## 🤝 Contributing

欢迎提交 Issue 和 Pull Request。

提交代码前，请确保：

```bash
cmake --build build
```

能够正常完成，并保持代码格式统一。

## 📄 License

本项目使用 MIT License。
