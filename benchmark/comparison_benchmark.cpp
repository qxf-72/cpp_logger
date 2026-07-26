#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/Logger.h>
#include <quill/core/MacroMetadata.h>
#include <quill/sinks/FileSink.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

#include "Logger.h"

namespace {
namespace fs = std::filesystem;

constexpr std::chrono::seconds kPeriodicFlushInterval{1};
constexpr std::size_t kQuillQueueBytesPerProducer = 256 * 1024;

// Quill 的队列为“每个生产者线程一个 SPSC 队列”，容量单位是字节；
// cpp_logger 则使用单个 MPMC 风格的记录队列。这里固定 Quill 每线程 256 KiB，
// 并让 cpp_logger 的记录容量随生产者数量同比例放大，避免多线程时一方的总缓冲区
// 反而保持不变。两者的容量单位不同，因此文档中只比较过载语义，不将容量视为严格等价。
#if !defined(LOGGER_QUILL_QUEUE_MODE)
#error "LOGGER_QUILL_QUEUE_MODE must be set by CMake"
#endif

#if LOGGER_QUILL_QUEUE_MODE == 1
struct QuillFrontendOptions {
  // Quill v9 在 MinGW 下销毁 BoundedBlocking 线程局部队列会触发运行时崩溃。
  // 可靠模式改用 Quill 默认的 UnboundedBlocking：它保证不丢日志，缓冲区可按需扩容。
  // 因而本 Profile 对齐的是“可靠交付”语义，而不是两种队列的容量实现。
  static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedBlocking;
  static constexpr std::size_t initial_queue_capacity = kQuillQueueBytesPerProducer;
  static constexpr std::uint32_t blocking_queue_retry_interval_ns = 800;
  static constexpr std::size_t unbounded_queue_max_capacity = 64 * 1024 * 1024;
  static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
};
#elif LOGGER_QUILL_QUEUE_MODE == 2
struct QuillFrontendOptions {
  static constexpr quill::QueueType queue_type = quill::QueueType::BoundedDropping;
  static constexpr std::size_t initial_queue_capacity = kQuillQueueBytesPerProducer;
  static constexpr std::uint32_t blocking_queue_retry_interval_ns = 800;
  static constexpr std::size_t unbounded_queue_max_capacity = kQuillQueueBytesPerProducer;
  static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
};
#else
#error "LOGGER_QUILL_QUEUE_MODE must be 1 (blocking) or 2 (dropping)"
#endif

using QuillFrontend = quill::FrontendImpl<QuillFrontendOptions>;

struct BenchmarkOptions {
  std::size_t threadCount{4};
  // 默认单轮持续超过 1 秒，确保 Periodic 1 s 的刷新策略能进入测量区间。
  std::size_t messagesPerThread{250000};
  std::size_t payloadSize{128};
  std::size_t runs{3};
  std::size_t cppQueueCapacityPerProducer{2048};
  std::size_t writeBatchSize{LoggerConfig::kDefaultWriteBatchSize};
  fs::path outputDirectory{"comparison_benchmark_logs"};
  bool keepLogs{false};
  // 留空时运行全部 Profile；该选项可重复，用于拆分耗时较长的正式测量。
  std::vector<std::string> selectedProfiles;
  std::string implementation{"all"};
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

// 一个 Profile 表示两套库可以解释为同一过载语义的一组配置。
// Quill 没有 DropOldest 队列，因此该项目特性不放入横向性能表，避免伪造等价关系。
struct BenchmarkProfile {
  std::string_view id;
  std::string_view scenario;
  std::string_view cppQueuePolicy;
  std::string_view quillQueuePolicy;
  OverflowPolicy cppOverflowPolicy;
};

void printUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n\n"
            << "This program compares one semantically aligned cpp_logger and Quill profile.\n\n"
            << "Options:\n"
            << "  --threads <N>              Producer thread count (default: 4)\n"
            << "  --messages <N>             Messages per producer (default: 250000)\n"
            << "  --payload <N>              Message payload size in bytes (default: 128)\n"
            << "  --runs <N>                 Repeated runs per profile (default: 3)\n"
            << "  --cpp-queue-per-thread <N> cpp_logger record capacity per producer "
               "(default: 2048)\n"
            << "  --batch-size <N>           cpp_logger writer batch size (default: 256)\n"
            << "  --output <PATH>            Temporary output directory\n"
            << "  --keep-logs                Keep log files after each run\n"
            << "  --profile <ID>             Run this executable's profile (repeatable)\n"
#if LOGGER_QUILL_QUEUE_MODE == 1
            << "                             ID: reliable_periodic\n"
#else
            << "                             ID: discard_new\n"
#endif
            << "  --implementation <X>       all, cpp_logger, or quill (default: all)\n"
            << "  --help                     Show this help message\n";
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
    } else if (argument == "--cpp-queue-per-thread") {
      options.cppQueueCapacityPerProducer = parsePositiveSize(value, argument);
    } else if (argument == "--batch-size") {
      options.writeBatchSize = parsePositiveSize(value, argument);
    } else if (argument == "--output") {
      options.outputDirectory = std::string(value);
    } else if (argument == "--profile") {
      options.selectedProfiles.emplace_back(value);
    } else if (argument == "--implementation") {
      options.implementation = value;
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }

  if (options.threadCount > std::numeric_limits<std::size_t>::max() / options.messagesPerThread) {
    throw std::invalid_argument("threads multiplied by messages is too large");
  }
  if (options.threadCount >
      std::numeric_limits<std::size_t>::max() / options.cppQueueCapacityPerProducer) {
    throw std::invalid_argument("threads multiplied by cpp queue capacity is too large");
  }
  if (options.implementation != "all" && options.implementation != "cpp_logger" &&
      options.implementation != "quill") {
    throw std::invalid_argument("--implementation must be all, cpp_logger, or quill");
  }
  return options;
}

std::size_t cppQueueCapacity(const BenchmarkOptions& options) {
  return options.threadCount * options.cppQueueCapacityPerProducer;
}

fs::path makeRunDirectory(const BenchmarkOptions& options, const BenchmarkProfile& profile,
                          std::string_view implementation, std::size_t runIndex) {
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory = options.outputDirectory /
                             (std::string(profile.id) + "_" + std::string(implementation) +
                              "_run_" + std::to_string(runIndex) + "_" + std::to_string(timestamp));
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

// Quill 会在线程退出时销毁线程本地 SPSC 队列。Windows MinGW 与 Quill v9 的这条析构路径
// 存在兼容性问题，因此 Quill 基准中的生产者在记录提交完成后先保持存活，待文件排空后再
// 由正常平台 join；MinGW 在所有结果已输出后通过 _Exit() 结束整个基准进程，避开有问题的
// 线程局部析构。这个分支只影响基准工具，不会进入 logger 库。
class QuillProducerGroup {
 public:
  QuillProducerGroup() = default;

  template <typename LogFunction>
  static std::unique_ptr<QuillProducerGroup> start(const BenchmarkOptions& options,
                                                   LogFunction&& logMessage) {
    auto group = std::make_unique<QuillProducerGroup>();
    group->workers_.reserve(options.threadCount);

    try {
      for (std::size_t index = 0; index < options.threadCount; ++index) {
        group->workers_.emplace_back([groupPtr = group.get(), &options, &logMessage] {
          {
            std::unique_lock lock(groupPtr->startMutex_);
            ++groupPtr->readyWorkers_;
            groupPtr->readyCondition_.notify_one();
            groupPtr->startCondition_.wait(lock, [groupPtr] { return groupPtr->startWorkers_; });
          }

          const std::string message(options.payloadSize, 'x');
          for (std::size_t messageIndex = 0; messageIndex < options.messagesPerThread;
               ++messageIndex) {
            logMessage(message);
          }

          {
            std::scoped_lock lock(groupPtr->completionMutex_);
            ++groupPtr->completedWorkers_;
          }
          groupPtr->completionCondition_.notify_one();

          std::unique_lock lock(groupPtr->releaseMutex_);
          groupPtr->releaseCondition_.wait(lock, [groupPtr] { return groupPtr->releaseWorkers_; });
        });
      }
    } catch (...) {
      group->releaseAndJoin();
      throw;
    }

    {
      std::unique_lock lock(group->startMutex_);
      group->readyCondition_.wait(
          lock, [&group, &options] { return group->readyWorkers_ == options.threadCount; });
      group->startWorkers_ = true;
    }
    group->startTime_ = std::chrono::steady_clock::now();
    group->startCondition_.notify_all();

    {
      std::unique_lock lock(group->completionMutex_);
      group->completionCondition_.wait(
          lock, [&group, &options] { return group->completedWorkers_ == options.threadCount; });
    }
    group->producersDone_ = std::chrono::steady_clock::now();
    return group;
  }

  QuillProducerGroup(const QuillProducerGroup&) = delete;
  QuillProducerGroup& operator=(const QuillProducerGroup&) = delete;

  ~QuillProducerGroup() {
    releaseAndJoin();
  }

  [[nodiscard]] std::chrono::steady_clock::time_point startTime() const noexcept {
    return startTime_;
  }

  [[nodiscard]] std::chrono::steady_clock::time_point producersDone() const noexcept {
    return producersDone_;
  }

  void releaseAndJoin() noexcept {
    {
      std::scoped_lock lock(releaseMutex_);
      releaseWorkers_ = true;
    }
    releaseCondition_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

 private:
  std::mutex startMutex_;
  std::condition_variable readyCondition_;
  std::condition_variable startCondition_;
  std::size_t readyWorkers_{0};
  bool startWorkers_{false};

  std::mutex completionMutex_;
  std::condition_variable completionCondition_;
  std::size_t completedWorkers_{0};

  std::mutex releaseMutex_;
  std::condition_variable releaseCondition_;
  bool releaseWorkers_{false};
  std::vector<std::thread> workers_;
  std::chrono::steady_clock::time_point startTime_{};
  std::chrono::steady_clock::time_point producersDone_{};
};

void retainQuillProducerGroupForMinGwExit(std::unique_ptr<QuillProducerGroup> group) {
  // 进程级堆对象故意不析构；main() 在输出结果后使用 _Exit() 回收整个进程资源。
  static auto* groups = new std::vector<std::unique_ptr<QuillProducerGroup>>;
  groups->push_back(std::move(group));
}

RunResult runCppLogger(const BenchmarkOptions& options, const BenchmarkProfile& profile,
                       std::size_t runIndex) {
  const fs::path directory = makeRunDirectory(options, profile, "cpp_logger", runIndex);
  auto& logger = Logger::instance();
  logger.stop();

  try {
    LoggerConfig config;
    config.basePath = directory / "cpp_logger";
    config.minLevel = LogLevel::INFO;
    config.maxFileSize = std::numeric_limits<std::size_t>::max() / 2;
    config.queueCapacity = cppQueueCapacity(options);
    config.overflowPolicy = profile.cppOverflowPolicy;
    config.writeBatchSize = options.writeBatchSize;
    config.flushPolicy = FlushPolicy::Periodic;
    config.flushInterval = kPeriodicFlushInterval;
    // 本基准只写 INFO，关闭按级别刷新的附加条件，使刷新时机只由 Profile 决定。
    config.flushAtOrAbove = std::nullopt;
    if (!logger.init(config)) {
      throw std::runtime_error("unable to initialize cpp_logger");
    }

    const auto startTime = runProducers(options, [](const std::string& message) {
      Logger::instance().logStatic(LogLevel::INFO, __FILE__, __LINE__, message);
    });
    const auto producersDone = std::chrono::steady_clock::now();
    const std::uint64_t dropped = logger.droppedCount();
    // stop() 会关闭输入、排空已接收记录并刷新文件；结束时间因此代表端到端完成时间。
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

// 不使用 Quill 的非前缀 LOG_INFO 宏，而是直接调用其公开的入队接口：
// 这样可获得 bool 返回值，精确统计 BoundedDropping 队列拒绝的新记录数量。
template <typename QuillLogger>
bool enqueueQuill(QuillLogger* logger, const std::string& message) {
  static constexpr quill::MacroMetadata metadata{
      __FILE__ ":0", "", "{}", nullptr, quill::LogLevel::Info, quill::MacroMetadata::Event::Log};
  return logger->template log_statement<false, false>(quill::LogLevel::None, &metadata, message);
}

template <typename QuillFrontend>
RunResult runQuillProfile(const BenchmarkOptions& options, const BenchmarkProfile& profile,
                          std::size_t runIndex) {
  const fs::path directory = makeRunDirectory(options, profile, "quill", runIndex);
  typename QuillFrontend::logger_t* logger = nullptr;
  std::unique_ptr<QuillProducerGroup> producers;

  try {
    const fs::path filename = directory / "quill.log";
    // Quill 的 SinkManager 将第一个参数同时作为 Sink 名和 FileSink 构造参数，
    // 因此这里直接使用唯一的完整文件路径，避免额外传入一次 filename。
    const std::string sinkName = filename.string();
    const std::string loggerName = "quill_logger_" + std::string(profile.id) + "_" +
                                   std::to_string(runIndex) + "_" + filename.string();

    quill::FileSinkConfig sinkConfig;
    sinkConfig.set_open_mode('w');
    auto sink = QuillFrontend::template create_or_get_sink<quill::FileSink>(
        sinkName, sinkConfig, quill::FileEventNotifier{});
    const quill::PatternFormatterOptions formatter{
        "[%(time)][%(log_level)][tid:%(thread_id)][%(source_location)] %(message)",
        "%Y-%m-%d %H:%M:%S.%Qms", quill::Timezone::LocalTime};
    logger = QuillFrontend::create_or_get_logger(loggerName, std::move(sink), formatter,
                                                 quill::ClockSourceType::System);
    logger->set_log_level(quill::LogLevel::Info);

    std::atomic<std::uint64_t> dropped{0};
    producers = QuillProducerGroup::start(options, [&logger, &dropped](const std::string& message) {
      if (!enqueueQuill(logger, message)) {
        dropped.fetch_add(1, std::memory_order_relaxed);
      }
    });
    const auto startTime = producers->startTime();
    const auto producersDone = producers->producersDone();

    // flush_log() 等待此前记录完成格式化和文件 flush；随后同步删除 logger，确保其所有
    // 生产者队列与后台缓存均已排空后才停止端到端计时。
    logger->flush_log();
    QuillFrontend::remove_logger_blocking(logger);
    logger = nullptr;
    const auto finished = std::chrono::steady_clock::now();
#if defined(__MINGW32__)
    retainQuillProducerGroupForMinGwExit(std::move(producers));
#else
    producers->releaseAndJoin();
#endif
    removeRunDirectory(directory, options.keepLogs);
    return {std::chrono::duration<double>(producersDone - startTime).count(),
            std::chrono::duration<double>(finished - startTime).count(),
            dropped.load(std::memory_order_relaxed)};
  } catch (...) {
    if (producers) {
#if defined(__MINGW32__)
      retainQuillProducerGroupForMinGwExit(std::move(producers));
#else
      producers->releaseAndJoin();
#endif
    }
    if (logger != nullptr) {
      QuillFrontend::remove_logger_blocking(logger);
    }
    removeRunDirectory(directory, options.keepLogs);
    throw;
  }
}

RunResult runQuill(const BenchmarkOptions& options, const BenchmarkProfile& profile,
                   std::size_t runIndex) {
  return runQuillProfile<QuillFrontend>(options, profile, runIndex);
}

template <typename RunFunction>
AggregateResult runRepeated(const BenchmarkOptions& options, RunFunction&& runOnce) {
  AggregateResult aggregate;
  for (std::size_t runIndex = 1; runIndex <= options.runs; ++runIndex) {
    const RunResult result = runOnce(runIndex);
    aggregate.producerSeconds += result.producerSeconds;
    aggregate.endToEndSeconds += result.endToEndSeconds;
    aggregate.dropped += static_cast<double>(result.dropped);
  }
  aggregate.producerSeconds /= static_cast<double>(options.runs);
  aggregate.endToEndSeconds /= static_cast<double>(options.runs);
  aggregate.dropped /= static_cast<double>(options.runs);
  return aggregate;
}

void printResult(const BenchmarkOptions& options, const BenchmarkProfile& profile,
                 std::string_view implementation, std::string_view queuePolicy,
                 std::string_view queueCapacity, const AggregateResult& result) {
  const std::size_t attempted = options.threadCount * options.messagesPerThread;
  const double accepted = std::max(0.0, static_cast<double>(attempted) - result.dropped);
  const double producerRate = static_cast<double>(attempted) / result.producerSeconds;
  const double endToEndRate = accepted / result.endToEndSeconds;
  const double droppedRate = result.dropped * 100.0 / static_cast<double>(attempted);

  std::cout << "| " << profile.scenario << " | " << implementation << " | " << queuePolicy << " | "
            << queueCapacity << " | Periodic 1 s | " << std::fixed << std::setprecision(0)
            << producerRate << " | " << endToEndRate << " | " << std::setprecision(4) << droppedRate
            << "% |\n"
            << std::flush;
}

bool shouldRunProfile(const BenchmarkOptions& options, std::string_view profileId) {
  return options.selectedProfiles.empty() ||
         std::find(options.selectedProfiles.begin(), options.selectedProfiles.end(), profileId) !=
             options.selectedProfiles.end();
}

bool shouldRunImplementation(const BenchmarkOptions& options, std::string_view implementation) {
  return options.implementation == "all" || options.implementation == implementation;
}

void startQuillBackend() {
  quill::BackendOptions backendOptions;
  backendOptions.sink_min_flush_interval = kPeriodicFlushInterval;
  // Windows 的实际调度精度远大于 Quill 默认的 500 ns 空闲等待，默认设置会把后台
  // 消费唤醒放大到毫秒级。性能压测改为忙等并在空闲时 yield，避免计入该平台调度误差。
  backendOptions.sleep_duration = std::chrono::nanoseconds::zero();
  backendOptions.enable_yield_when_idle = true;
  // cpp_logger 不会在队列满时向 stderr 写诊断；关闭 Quill 的默认文本通知，避免它影响
  // 丢新模式下的写盘吞吐。丢弃数由 enqueueQuill() 的返回值逐条统计。
  backendOptions.error_notifier = [](const std::string&) {};
  quill::Backend::start(backendOptions);
}
}  // namespace

int main(int argc, char* argv[]) {
  bool quillBackendStarted = false;
  try {
    const BenchmarkOptions options = parseOptions(argc, argv);
#if LOGGER_QUILL_QUEUE_MODE == 1
    const std::vector<BenchmarkProfile> profiles = {
        {"reliable_periodic", "Reliable / periodic", "Block", "UnboundedBlocking (max 64 MiB)",
         OverflowPolicy::Block},
    };
#else
    const std::vector<BenchmarkProfile> profiles = {
        {"discard_new", "Loss-tolerant / discard newest", "DropNewest", "BoundedDropping",
         OverflowPolicy::DropNewest},
    };
#endif

    for (const std::string& selectedProfile : options.selectedProfiles) {
      const bool found = std::any_of(profiles.begin(), profiles.end(),
                                     [&selectedProfile](const BenchmarkProfile& profile) {
                                       return profile.id == selectedProfile;
                                     });
      if (!found) {
        throw std::invalid_argument("unknown --profile value: " + selectedProfile);
      }
    }

    if (shouldRunImplementation(options, "quill")) {
      startQuillBackend();
      quillBackendStarted = true;
    }

    const std::string cppCapacity = std::to_string(cppQueueCapacity(options)) + " records total";
#if LOGGER_QUILL_QUEUE_MODE == 1
    const std::string quillCapacity = "256 KiB initial / 64 MiB max per producer";
#else
    const std::string quillCapacity =
        std::to_string(kQuillQueueBytesPerProducer / 1024) + " KiB / producer";
#endif
    std::cout
        << "# cpp_logger vs Quill semantically aligned asynchronous file benchmark\n"
        << "# quill_version=" << quill::VersionMajor << '.' << quill::VersionMinor << '.'
        << quill::VersionPatch << ", threads=" << options.threadCount
        << ", messages_per_thread=" << options.messagesPerThread
        << ", payload_bytes=" << options.payloadSize << ", runs=" << options.runs << '\n'
        << "# cpp_logger_queue=" << cppCapacity
        << ", cpp_logger_batch_size=" << options.writeBatchSize << ", quill_queue=" << quillCapacity
        << '\n'
        << "# Both implementations write text logs with local-time millisecond timestamps and "
           "Periodic 1 s flushing.\n"
        << "# Producer throughput counts all attempted submissions; end-to-end throughput counts "
           "records retained after overflow handling and completely flushed to the file.\n\n"
        << "| Scenario | Implementation | Queue policy | Queue capacity | Flush policy | "
           "Producer logs/s | End-to-end logs/s | Drop rate |\n"
        << "| --- | --- | --- | --- | --- | ---: | ---: | ---: |\n"
        << std::flush;

    for (const BenchmarkProfile& profile : profiles) {
      if (!shouldRunProfile(options, profile.id)) {
        continue;
      }
      if (shouldRunImplementation(options, "cpp_logger")) {
        const AggregateResult cppLoggerResult = runRepeated(options, [&](std::size_t runIndex) {
          return runCppLogger(options, profile, runIndex);
        });
        printResult(options, profile, "cpp_logger", profile.cppQueuePolicy, cppCapacity,
                    cppLoggerResult);
      }
      if (shouldRunImplementation(options, "quill")) {
        const AggregateResult quillResult = runRepeated(
            options, [&](std::size_t runIndex) { return runQuill(options, profile, runIndex); });
        printResult(options, profile, "Quill", profile.quillQueuePolicy, quillCapacity,
                    quillResult);
      }
    }

    // Quill 在 Windows MinGW 的线程局部析构与 atexit 清理顺序存在兼容性问题。
    // 这里的每一轮已通过 flush_log() 和 remove_logger_blocking() 完成排空与文件关闭，
    // 所以仅在该工具链上显式停止后台线程后快速退出，跳过重复的全局析构。
#if defined(__MINGW32__)
    if (quillBackendStarted) {
      quill::Backend::stop();
      std::cout.flush();
      std::_Exit(0);
    }
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "comparison benchmark failed: " << error.what() << '\n';
#if defined(__MINGW32__)
    if (quillBackendStarted) {
      quill::Backend::stop();
      std::cerr.flush();
      std::_Exit(1);
    }
#endif
    return 1;
  }
}
