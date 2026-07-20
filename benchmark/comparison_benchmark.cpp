// Include cpp_logger first: spdlog may indirectly include Windows headers that define ERROR.
#include "Logger.h"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <spdlog/version.h>

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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr std::size_t kWriterThreadCount = 1;
constexpr std::chrono::seconds kPeriodicFlushInterval{1};

struct BenchmarkOptions {
  std::size_t threadCount{4};
  // 默认单轮持续超过 1 秒，使 Periodic 1 s 的行为能够进入测量区间。
  std::size_t messagesPerThread{250000};
  std::size_t payloadSize{128};
  std::size_t runs{3};
  std::size_t queueCapacity{8192};
  std::size_t writeBatchSize{LoggerConfig::kDefaultWriteBatchSize};
  fs::path outputDirectory{"comparison_benchmark_logs"};
  bool keepLogs{false};
};

struct RunResult {
  double producerSeconds{0.0};
  double endToEndSeconds{0.0};
  std::uint64_t dropped{0};
};

struct AggregateResult {
  double producerSeconds{0.0};
  double endToEndSeconds{0.0};
  double dropped{0.0};
};

enum class Implementation { CppLogger, Spdlog };

struct BenchmarkMode {
  std::string_view name;
  std::string_view queuePolicy;
  std::string_view flushPolicy;
  Implementation implementation;
  OverflowPolicy overflowPolicy{OverflowPolicy::Block};
  FlushPolicy flushPolicyValue{FlushPolicy::Periodic};
};

void printUsage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n\n"
      << "This program runs four predefined profiles: reliable, balanced, producer-low-latency,"
         " and spdlog reference.\n\n"
      << "Options:\n"
      << "  --threads <N>         Producer thread count (default: 4)\n"
      << "  --messages <N>        Messages per producer (default: 250000)\n"
      << "  --payload <N>         Message payload size in bytes (default: 128)\n"
      << "  --runs <N>            Repeated runs per profile (default: 3)\n"
      << "  --queue-capacity <N>  Async queue capacity (default: 8192)\n"
      << "  --batch-size <N>      cpp_logger writer batch size (default: 256)\n"
      << "  --output <PATH>       Temporary output directory\n"
      << "  --keep-logs           Keep log files after each run\n"
      << "  --help                Show this help message\n";
}

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
    } else if (argument == "--queue-capacity") {
      options.queueCapacity = parsePositiveSize(value, argument);
    } else if (argument == "--batch-size") {
      options.writeBatchSize = parsePositiveSize(value, argument);
    } else if (argument == "--output") {
      options.outputDirectory = std::string(value);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }

  if (options.threadCount > std::numeric_limits<std::size_t>::max() / options.messagesPerThread) {
    throw std::invalid_argument("threads multiplied by messages is too large");
  }
  return options;
}

fs::path makeRunDirectory(const BenchmarkOptions& options, std::string_view mode,
                          std::size_t runIndex) {
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      options.outputDirectory /
      (std::string(mode) + "_run_" + std::to_string(runIndex) + "_" + std::to_string(timestamp));
  std::error_code error;
  fs::create_directories(directory, error);
  if (error) {
    throw std::runtime_error("unable to create benchmark directory: " + error.message());
  }
  return directory;
}

void removeRunDirectory(const fs::path& directory, bool keepLogs) {
  if (keepLogs) {
    return;
  }
  std::error_code error;
  fs::remove_all(directory, error);
  if (error) {
    std::cerr << "warning: unable to remove benchmark logs: " << error.message() << '\n';
  }
}

template <typename LogFunction>
std::chrono::steady_clock::time_point runProducers(const BenchmarkOptions& options,
                                                   LogFunction&& logMessage) {
  std::mutex startMutex;
  std::condition_variable readyCondition;
  std::condition_variable startCondition;
  std::size_t readyWorkers = 0;
  bool startWorkers = false;
  const std::string message(options.payloadSize, 'x');
  std::vector<std::thread> workers;
  workers.reserve(options.threadCount);

  const auto joinWorkers = [&workers] {
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  };

  try {
    for (std::size_t index = 0; index < options.threadCount; ++index) {
      workers.emplace_back([&] {
        {
          std::unique_lock lock(startMutex);
          ++readyWorkers;
          readyCondition.notify_one();
          startCondition.wait(lock, [&] { return startWorkers; });
        }
        for (std::size_t messageIndex = 0; messageIndex < options.messagesPerThread;
             ++messageIndex) {
          logMessage(message);
        }
      });
    }
  } catch (...) {
    {
      std::scoped_lock lock(startMutex);
      startWorkers = true;
    }
    startCondition.notify_all();
    joinWorkers();
    throw;
  }

  {
    std::unique_lock lock(startMutex);
    readyCondition.wait(lock, [&] { return readyWorkers == options.threadCount; });
    startWorkers = true;
  }
  const auto startTime = std::chrono::steady_clock::now();
  startCondition.notify_all();
  joinWorkers();
  return startTime;
}

RunResult runCppLogger(const BenchmarkOptions& options, const BenchmarkMode& mode,
                       std::size_t runIndex) {
  const fs::path directory = makeRunDirectory(options, mode.name, runIndex);
  auto& logger = Logger::instance();
  logger.stop();

  try {
    LoggerConfig config;
    config.basePath = directory / "cpp_logger";
    config.minLevel = LogLevel::INFO;
    config.maxFileSize = std::numeric_limits<std::size_t>::max() / 2;
    config.queueCapacity = options.queueCapacity;
    config.overflowPolicy = mode.overflowPolicy;
    config.writeBatchSize = options.writeBatchSize;
    config.flushPolicy = mode.flushPolicyValue;
    config.flushInterval = kPeriodicFlushInterval;
    // 本基准只写 INFO，关闭该项让刷新策略完全由模式决定。
    config.flushAtOrAbove = std::nullopt;
    if (!logger.init(config)) {
      throw std::runtime_error("unable to initialize cpp_logger");
    }

    const auto startTime = runProducers(options, [](const std::string& message) {
      Logger::instance().logStatic(LogLevel::INFO, __FILE__, __LINE__, message);
    });
    const auto producersDone = std::chrono::steady_clock::now();
    const std::uint64_t dropped = logger.droppedCount();
    logger.stop();
    const auto finished = std::chrono::steady_clock::now();
    removeRunDirectory(directory, options.keepLogs);
    return {std::chrono::duration<double>(producersDone - startTime).count(),
            std::chrono::duration<double>(finished - startTime).count(), dropped};
  } catch (...) {
    logger.stop();
    removeRunDirectory(directory, options.keepLogs);
    throw;
  }
}

RunResult runSpdlog(const BenchmarkOptions& options, const BenchmarkMode& mode,
                    std::size_t runIndex) {
  const fs::path directory = makeRunDirectory(options, mode.name, runIndex);
  const fs::path filename = directory / "spdlog.log";
  spdlog::shutdown();

  try {
    spdlog::init_thread_pool(options.queueCapacity, kWriterThreadCount);
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename.string(), true);
    auto logger = std::make_shared<spdlog::async_logger>(
        "spdlog_comparison_" + std::to_string(runIndex), sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    logger->set_level(spdlog::level::info);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l][tid:%t][%s:%#] %v");
    logger->flush_on(spdlog::level::off);
    spdlog::register_logger(logger);
    spdlog::flush_every(kPeriodicFlushInterval);

    const spdlog::source_loc source{__FILE__, __LINE__, SPDLOG_FUNCTION};
    const auto startTime = runProducers(options, [&logger, source](const std::string& message) {
      logger->log(source, spdlog::level::info,
                  spdlog::string_view_t(message.data(), message.size()));
    });
    const auto producersDone = std::chrono::steady_clock::now();

    // flush() 以 FIFO 控制消息排在所有日志之后；shutdown() 回收后台线程并等待它处理完。
    logger->flush();
    spdlog::drop(logger->name());
    logger.reset();
    spdlog::shutdown();
    sink->flush();
    sink.reset();

    const auto finished = std::chrono::steady_clock::now();
    removeRunDirectory(directory, options.keepLogs);
    return {std::chrono::duration<double>(producersDone - startTime).count(),
            std::chrono::duration<double>(finished - startTime).count(), 0};
  } catch (...) {
    spdlog::shutdown();
    removeRunDirectory(directory, options.keepLogs);
    throw;
  }
}

AggregateResult runMode(const BenchmarkOptions& options, const BenchmarkMode& mode) {
  AggregateResult aggregate;
  for (std::size_t runIndex = 1; runIndex <= options.runs; ++runIndex) {
    const RunResult result = mode.implementation == Implementation::CppLogger
                                 ? runCppLogger(options, mode, runIndex)
                                 : runSpdlog(options, mode, runIndex);
    aggregate.producerSeconds += result.producerSeconds;
    aggregate.endToEndSeconds += result.endToEndSeconds;
    aggregate.dropped += static_cast<double>(result.dropped);
  }
  aggregate.producerSeconds /= static_cast<double>(options.runs);
  aggregate.endToEndSeconds /= static_cast<double>(options.runs);
  aggregate.dropped /= static_cast<double>(options.runs);
  return aggregate;
}

void printResult(const BenchmarkOptions& options, const BenchmarkMode& mode,
                 const AggregateResult& result) {
  const std::size_t attempted = options.threadCount * options.messagesPerThread;
  const double accepted = static_cast<double>(attempted) - result.dropped;
  const double producerRate = static_cast<double>(attempted) / result.producerSeconds;
  const double endToEndRate = accepted / result.endToEndSeconds;
  const double droppedRate = result.dropped * 100.0 / static_cast<double>(attempted);

  std::cout << "| " << mode.name << " | " << mode.queuePolicy << " | " << mode.flushPolicy << " | "
            << std::fixed << std::setprecision(0) << producerRate << " | " << endToEndRate << " | "
            << std::setprecision(4) << droppedRate << "% |\n";
}
}  // namespace

int main(int argc, char* argv[]) {
  try {
    const BenchmarkOptions options = parseOptions(argc, argv);
    const std::vector<BenchmarkMode> modes = {
        {"Reliable (cpp_logger)", "Block", "EveryBatch", Implementation::CppLogger,
         OverflowPolicy::Block, FlushPolicy::EveryBatch},
        {"Balanced (cpp_logger)", "Block", "Periodic 1 s", Implementation::CppLogger,
         OverflowPolicy::Block, FlushPolicy::Periodic},
        {"Producer-low-latency (cpp_logger)", "DropNewest", "Periodic 1 s",
         Implementation::CppLogger, OverflowPolicy::DropNewest, FlushPolicy::Periodic},
        {"spdlog reference", "Block", "Periodic 1 s (same as balanced)", Implementation::Spdlog},
    };

    std::cout << "# cpp_logger vs spdlog asynchronous file benchmark\n"
              << "# threads=" << options.threadCount
              << ", messages_per_thread=" << options.messagesPerThread
              << ", payload_bytes=" << options.payloadSize << ", runs=" << options.runs
              << ", queue_capacity=" << options.queueCapacity
              << ", cpp_logger_batch_size=" << options.writeBatchSize << '\n'
              << "# spdlog_version=" << SPDLOG_VER_MAJOR << '.' << SPDLOG_VER_MINOR << '.'
              << SPDLOG_VER_PATCH << ", async_worker_threads=" << kWriterThreadCount << '\n'
              << "# producer throughput counts attempted submissions; end-to-end throughput counts "
                 "accepted records.\n\n"
              << "| Mode | Queue policy | Flush policy | Producer logs/s | End-to-end logs/s | "
                 "Drop rate |\n"
              << "| --- | --- | --- | ---: | ---: | ---: |\n";

    for (const BenchmarkMode& mode : modes) {
      printResult(options, mode, runMode(options, mode));
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "comparison benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
