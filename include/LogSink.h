#ifndef LOG_SINK_H
#define LOG_SINK_H

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// 控制台 Sink 的输出目标。
enum class ConsoleStream { Stdout, Stderr };

// 格式化后的日志输出端。Logger 只在后台线程中调用这些接口。
class LogSink {
 public:
  using Batch = std::vector<std::string>;

  virtual ~LogSink() = default;

  // 一批记录不包含换行符；各 Sink 负责采用自己的方式输出记录分隔符。
  virtual void writeBatch(const Batch& records) = 0;
  virtual void flush() = 0;

  // 默认没有需要释放的资源；文件类自定义 Sink 可重写此函数。
  virtual void close() noexcept {}
};

namespace detail {

// 内置 Sink 的创建细节只供 Logger 实现使用；普通调用方无需直接调用。
std::shared_ptr<LogSink> makeFileSink(const std::filesystem::path& basePath,
                                      std::size_t maxFileSize, std::size_t writeBatchSize);
std::shared_ptr<LogSink> makeConsoleSink(ConsoleStream stream, std::size_t writeBatchSize);

}  // namespace detail

#endif
