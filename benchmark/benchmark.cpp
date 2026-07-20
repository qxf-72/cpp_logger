#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "Logger.h"

namespace {
namespace fs = std::filesystem;

// 压测参数；默认值兼顾可观察性和较短的运行时间。
struct BenchmarkOptions {
  std::size_t threadCount{4};
  std::size_t messagesPerThread{50000};
  std::size_t payloadSize{128};
  std::size_t runs{3};
  std::size_t writeBatchSize{LoggerConfig::kDefaultWriteBatchSize};
  FlushPolicy flushPolicy{FlushPolicy::Periodic};
  std::size_t flushIntervalMilliseconds{
      static_cast<std::size_t>(LoggerConfig::kDefaultFlushInterval.count())};
  fs::path outputDirectory{"benchmark_logs"};
  bool keepLogs{false};
};

// 分别记录生产者完成提交，以及 Logger 完成排空和刷新所需的时间。
struct BenchmarkResult {
  double producerSeconds{0.0};
  double totalSeconds{0.0};
};

void printUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n\n"
            << "Options:\n"
            << "  --threads <N>    Producer thread count (default: 4)\n"
            << "  --messages <N>   Messages per producer (default: 50000)\n"
            << "  --payload <N>    Message payload size in bytes (default: 128)\n"
            << "  --runs <N>       Number of repeated runs (default: 3)\n"
            << "  --batch-size <N> Log records per writer batch (default: 256)\n"
            << "  --flush-policy <on-stop|periodic|every-batch> (default: periodic)\n"
            << "  --flush-interval-ms <N> Periodic flush interval in milliseconds (default: 1000)\n"
            << "  --output <PATH>  Directory for temporary log files (default: benchmark_logs)\n"
            << "  --keep-logs      Keep the log files after each run\n"
            << "  --help           Show this help message\n";
}

FlushPolicy parseFlushPolicy(std::string_view value) {
  if (value == "on-stop") {
    return FlushPolicy::OnStop;
  }
  if (value == "periodic") {
    return FlushPolicy::Periodic;
  }
  if (value == "every-batch") {
    return FlushPolicy::EveryBatch;
  }
  throw std::invalid_argument("invalid flush policy: " + std::string(value));
}

std::string_view flushPolicyName(FlushPolicy policy) {
  switch (policy) {
    case FlushPolicy::OnStop:
      return "on-stop";
    case FlushPolicy::Periodic:
      return "periodic";
    case FlushPolicy::EveryBatch:
      return "every-batch";
  }
  return "unknown";
}

// 使用 from_chars 避免命令行参数解析依赖区域设置或抛出转换异常。
std::size_t parsePositiveSize(std::string_view value, std::string_view option) {
  unsigned long long parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
      parsed > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("invalid value for " + std::string(option) + ": " +
                                std::string(value));
  }
  return static_cast<std::size_t>(parsed);
}

BenchmarkOptions parseOptions(int argc, char* argv[]) {
  BenchmarkOptions options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--help") {
      printUsage(argv[0]);
      std::exit(0);
    }
    if (argument == "--keep-logs") {
      options.keepLogs = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(argument));
    }

    const std::string_view value = argv[++index];
    if (argument == "--threads") {
      options.threadCount = parsePositiveSize(value, argument);
    } else if (argument == "--messages") {
      options.messagesPerThread = parsePositiveSize(value, argument);
    } else if (argument == "--payload") {
      options.payloadSize = parsePositiveSize(value, argument);
    } else if (argument == "--runs") {
      options.runs = parsePositiveSize(value, argument);
    } else if (argument == "--batch-size") {
      options.writeBatchSize = parsePositiveSize(value, argument);
    } else if (argument == "--flush-policy") {
      options.flushPolicy = parseFlushPolicy(value);
    } else if (argument == "--flush-interval-ms") {
      options.flushIntervalMilliseconds = parsePositiveSize(value, argument);
    } else if (argument == "--output") {
      options.outputDirectory = std::string(value);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }

  // 防止后续计算总日志数时发生整数溢出。
  if (options.threadCount > std::numeric_limits<std::size_t>::max() / options.messagesPerThread) {
    throw std::invalid_argument("threads multiplied by messages is too large");
  }
  if (options.flushIntervalMilliseconds >
      static_cast<std::size_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
    throw std::invalid_argument("flush interval is too large");
  }
  return options;
}

// 线程创建部分失败时，确保已创建的线程都被回收。
void joinWorkers(std::vector<std::thread>& workers) {
  for (auto& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

BenchmarkResult runOnce(const BenchmarkOptions& options, std::size_t runIndex) {
  // 每轮使用独立目录，既不会追加旧日志，也方便在压测结束后整体清理。
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path runDirectory = options.outputDirectory / ("run_" + std::to_string(runIndex) + "_" +
                                                           std::to_string(timestamp));
  std::error_code error;
  fs::create_directories(runDirectory, error);
  if (error) {
    throw std::runtime_error("unable to create benchmark directory: " + error.message());
  }

  const fs::path logBasePath = runDirectory / "app";
  // 取一个实际不可达到的上限，避免文件滚动干扰基础吞吐量测试。
  const std::size_t maxFileSize = std::numeric_limits<std::size_t>::max() / 2;
  LoggerConfig loggerConfig;
  loggerConfig.basePath = logBasePath;
  loggerConfig.minLevel = LogLevel::INFO;
  loggerConfig.maxFileSize = maxFileSize;
  loggerConfig.writeBatchSize = options.writeBatchSize;
  loggerConfig.flushPolicy = options.flushPolicy;
  loggerConfig.flushInterval = std::chrono::milliseconds(
      static_cast<std::chrono::milliseconds::rep>(options.flushIntervalMilliseconds));
  if (!Logger::instance().init(loggerConfig)) {
    throw std::runtime_error("unable to initialize logger");
  }

  std::mutex startMutex;
  // 将“工作线程已就绪”和“允许开始压测”分成两个条件变量，避免通知被其他工作线程消费。
  std::condition_variable readyCondition;
  std::condition_variable startCondition;
  std::size_t readyWorkers = 0;
  bool startWorkers = false;
  const std::string message(options.payloadSize, 'x');
  std::vector<std::thread> workers;
  workers.reserve(options.threadCount);

  try {
    for (std::size_t index = 0; index < options.threadCount; ++index) {
      workers.emplace_back([&] {
        {
          std::unique_lock lock(startMutex);
          ++readyWorkers;
          readyCondition.notify_one();
          // 所有生产者从同一时刻起跑，避免把线程创建时间算进吞吐量。
          startCondition.wait(lock, [&] { return startWorkers; });
        }

        // 使用固定长度正文，测量日志库本身的格式化、入队和写入开销。
        for (std::size_t messageIndex = 0; messageIndex < options.messagesPerThread;
             ++messageIndex) {
          LOG_INFO(message);
        }
      });
    }
  } catch (...) {
    // 唤醒可能正在等待起跑信号的线程，再统一回收。
    {
      std::scoped_lock lock(startMutex);
      startWorkers = true;
    }
    startCondition.notify_all();
    joinWorkers(workers);
    Logger::instance().stop();
    throw;
  }

  std::chrono::steady_clock::time_point startTime;
  {
    std::unique_lock lock(startMutex);
    readyCondition.wait(lock, [&] { return readyWorkers == options.threadCount; });
    // 生产者计时从所有线程均已就绪、收到统一起跑信号时开始。
    startTime = std::chrono::steady_clock::now();
    startWorkers = true;
  }
  startCondition.notify_all();

  joinWorkers(workers);
  // 此时所有业务线程均已完成 LOG_INFO 调用，得到生产者提交耗时。
  const auto producersDone = std::chrono::steady_clock::now();

  if (!Logger::instance().shouldLog(LogLevel::INFO)) {
    Logger::instance().stop();
    throw std::runtime_error("logger stopped while the benchmark was running");
  }

  // stop() 会关闭队列、等待后台线程排空日志并刷新文件，得到端到端耗时。
  Logger::instance().stop();
  const auto finished = std::chrono::steady_clock::now();

  if (!options.keepLogs) {
    // 清理不计入性能数据，避免测试目录持续占用磁盘空间。
    fs::remove_all(runDirectory, error);
    if (error) {
      std::cerr << "warning: unable to remove benchmark logs: " << error.message() << '\n';
    }
  }

  return {std::chrono::duration<double>(producersDone - startTime).count(),
          std::chrono::duration<double>(finished - startTime).count()};
}

void printResult(std::string_view label, const BenchmarkOptions& options,
                 const BenchmarkResult& result) {
  const std::size_t totalMessages = options.threadCount * options.messagesPerThread;
  const double producerRate = totalMessages / result.producerSeconds;
  const double totalRate = totalMessages / result.totalSeconds;

  // 使用 CSV，便于直接导入电子表格或交给脚本做多组参数对比。
  std::cout << label << ',' << options.threadCount << ',' << options.messagesPerThread << ','
            << options.payloadSize << ',' << options.writeBatchSize << ','
            << flushPolicyName(options.flushPolicy) << ',' << totalMessages << ',' << std::fixed
            << std::setprecision(6) << result.producerSeconds << ',' << result.totalSeconds << ','
            << std::setprecision(2) << producerRate << ',' << totalRate << '\n';
}
}  // namespace

int main(int argc, char* argv[]) {
  try {
    const BenchmarkOptions options = parseOptions(argc, argv);
    std::cout << "run,threads,messages_per_thread,payload_bytes,batch_size,flush_policy,"
                 "total_messages,producer_seconds,end_to_end_seconds,producer_logs_per_second,"
                 "end_to_end_logs_per_second\n";

    // 每轮单独输出，并在末尾输出平均结果。
    BenchmarkResult total;
    for (std::size_t runIndex = 1; runIndex <= options.runs; ++runIndex) {
      const BenchmarkResult result = runOnce(options, runIndex);
      total.producerSeconds += result.producerSeconds;
      total.totalSeconds += result.totalSeconds;
      printResult(std::to_string(runIndex), options, result);
    }

    total.producerSeconds /= static_cast<double>(options.runs);
    total.totalSeconds /= static_cast<double>(options.runs);
    printResult("average", options, total);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
