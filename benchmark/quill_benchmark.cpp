#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BenchmarkHarness.h"

namespace {

using benchmark_support::BenchmarkMetadata;
using benchmark_support::Clock;
using benchmark_support::CommonOptions;
using benchmark_support::RunContext;
using benchmark_support::RunMetrics;
using benchmark_support::RunPhase;

constexpr std::size_t kQueueBytesPerProducer = 256 * 1024;

#if !defined(LOGGER_QUILL_QUEUE_MODE)
#error "LOGGER_QUILL_QUEUE_MODE must be set by CMake"
#endif

// Quill 的队列类型属于 FrontendOptions 的编译期配置；因此两种策略必须构建成不同进程。
#if LOGGER_QUILL_QUEUE_MODE == 1
struct QuillFrontendOptions {
  static constexpr quill::QueueType queue_type = quill::QueueType::BoundedBlocking;
  static constexpr std::size_t initial_queue_capacity = kQueueBytesPerProducer;
  static constexpr std::uint32_t blocking_queue_retry_interval_ns = 800;
  static constexpr std::size_t unbounded_queue_max_capacity = kQueueBytesPerProducer;
  static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
};
constexpr std::string_view kProfileId{"block"};
constexpr std::string_view kQueuePolicy{"Block (BoundedBlocking)"};
#elif LOGGER_QUILL_QUEUE_MODE == 2
struct QuillFrontendOptions {
  static constexpr quill::QueueType queue_type = quill::QueueType::BoundedDropping;
  static constexpr std::size_t initial_queue_capacity = kQueueBytesPerProducer;
  static constexpr std::uint32_t blocking_queue_retry_interval_ns = 800;
  static constexpr std::size_t unbounded_queue_max_capacity = kQueueBytesPerProducer;
  static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
};
constexpr std::string_view kProfileId{"drop-newest"};
constexpr std::string_view kQueuePolicy{"DropNewest (BoundedDropping)"};
#else
#error "LOGGER_QUILL_QUEUE_MODE must be 1 (blocking) or 2 (dropping)"
#endif

using QuillFrontend = quill::FrontendImpl<QuillFrontendOptions>;
using QuillLogger = QuillFrontend::logger_t;

struct BenchmarkOptions {
  CommonOptions common;
};

void printUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n\n"
            << "Measure one Quill asynchronous file logging profile.\n\n"
            << "Common options:\n";
  benchmark_support::printCommonUsage(std::cout);
  std::cout << "\nThis executable profile: " << kProfileId << '\n'
            << "Queue: " << kQueueBytesPerProducer / 1024 << " KiB per producer\n"
            << "  --help                    Show this help message\n";
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
      options.common.keepLogs = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(argument));
    }

    const std::string_view value = argv[++index];
    if (!benchmark_support::parseCommonValueOption(options.common, argument, value)) {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }

  benchmark_support::validateCommonOptions(options.common);
  for (const std::string& selectedProfile : options.common.selectedProfiles) {
    if (selectedProfile != kProfileId) {
      throw std::invalid_argument("this executable only supports --profile " +
                                  std::string(kProfileId));
    }
  }
  return options;
}

// 使用 Quill 的宏接口作为实际生产者调用路径，和普通业务代码保持一致。
struct QuillSubmit {
  QuillLogger* logger{nullptr};

  void operator()(const std::string& message) const {
    QUILL_LOG_INFO(logger, "{}", message);
  }
};

struct QuillPrepare {
  void operator()() const {
    // 每个生产者线程的 SPSC 队列在计时屏障前预分配，不把首次分配算入吞吐。
    QuillFrontend::preallocate();
  }
};

using QuillProducerGroup = benchmark_support::ProducerGroup<QuillSubmit, QuillPrepare>;

void removeLoggerAfterFailure(QuillLogger*& logger) noexcept {
  if (logger == nullptr) {
    return;
  }

  try {
    logger->flush_log();
  } catch (...) {
    // 继续尝试移除 logger，以便后端线程和 Sink 不遗留到下一轮。
  }
  try {
    QuillFrontend::remove_logger_blocking(logger);
  } catch (...) {
    // 保留原始异常；main 会在最后停止 Backend。
  }
  logger = nullptr;
}

RunMetrics runOnce(const BenchmarkOptions& options, RunPhase phase, std::size_t runIndex) {
  RunContext context =
      benchmark_support::makeRunContext(options.common, "quill", kProfileId, phase, runIndex);
  QuillLogger* logger = nullptr;
  std::unique_ptr<QuillProducerGroup> producers;

  try {
    const std::filesystem::path filename = context.outputDirectory / "quill.log";
    quill::FileSinkConfig sinkConfig;
    sinkConfig.set_open_mode('w');
    // 用唯一完整路径作为 sink 名和文件名，避免 SinkManager 复用上一轮的输出对象。
    auto sink = QuillFrontend::create_or_get_sink<quill::FileSink>(filename.string(), sinkConfig,
                                                                   quill::FileEventNotifier{});
    const quill::PatternFormatterOptions formatter{
        "[%(time)][%(log_level)][tid:%(thread_id)][%(source_location)] %(message)",
        "%Y-%m-%d %H:%M:%S.%Qms", quill::Timezone::LocalTime};
    const std::string loggerName = "quill_benchmark_" + context.outputDirectory.filename().string();
    logger = QuillFrontend::create_or_get_logger(loggerName, std::move(sink), formatter,
                                                 quill::ClockSourceType::System);
    logger->set_log_level(quill::LogLevel::Info);

    producers = QuillProducerGroup::start(options.common, context.payload, QuillSubmit{logger},
                                          QuillPrepare{});
    const benchmark_support::ProducerTiming timing = producers->timing();

    // flush_log() 等待此前记录完成格式化和文件 flush；同步删除 logger 后才记录结束时间。
    logger->flush_log();
    QuillFrontend::remove_logger_blocking(logger);
    logger = nullptr;
    const Clock::time_point finished = Clock::now();

    // 生产者线程保持到排空之后才退出，避免其线程局部 SPSC 队列过早析构。
    producers->releaseAndJoin();
    const RunMetrics metrics = benchmark_support::finalizeRun(context, timing, finished);
    benchmark_support::cleanupRunDirectory(context, options.common.keepLogs);
    return metrics;
  } catch (...) {
    removeLoggerAfterFailure(logger);
    if (producers) {
      producers->releaseAndJoin();
    }
    benchmark_support::cleanupRunDirectory(context, options.common.keepLogs);
    throw;
  }
}

void startBackend(const CommonOptions& options) {
  quill::BackendOptions backendOptions;
  if (options.flushMode == benchmark_support::FlushMode::Periodic) {
    backendOptions.sink_min_flush_interval =
        std::chrono::milliseconds(options.flushIntervalMilliseconds);
  } else {
    // FinalDrain 的文件缓冲仅在每轮 flush_log() 时刷新，避免周期刷盘进入主测量口径。
    backendOptions.sink_min_flush_interval = std::chrono::hours(24);
  }
  // DropNewest 由输出 marker 校验统计，不向 stderr 输出诊断干扰文件吞吐。
  backendOptions.error_notifier = [](const std::string&) {};
  quill::Backend::start(backendOptions);
}

BenchmarkMetadata metadataFor(const BenchmarkOptions& options) {
  return {"Quill",
          std::to_string(quill::VersionMajor) + "." + std::to_string(quill::VersionMinor) + "." +
              std::to_string(quill::VersionPatch),
          std::string(kProfileId),
          std::string(kQueuePolicy),
          std::to_string(kQueueBytesPerProducer / 1024) + " KiB per producer",
          std::string(benchmark_support::flushModeName(options.common.flushMode))};
}

void printPreamble(const BenchmarkOptions& options) {
  std::cout << "# benchmark=Quill\n"
            << "# threads=" << options.common.threadCount
            << ", messages_per_thread=" << options.common.messagesPerThread
            << ", payload_bytes=" << options.common.payloadSize
            << ", warmups=" << options.common.warmupRuns
            << ", measured_runs=" << options.common.measuredRuns
            << ", queue_bytes_per_producer=" << kQueueBytesPerProducer << ", worker_threads=1\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  bool backendStarted = false;
  try {
    const BenchmarkOptions options = parseOptions(argc, argv);
    printPreamble(options);
    startBackend(options.common);
    backendStarted = true;

    const BenchmarkMetadata metadata = metadataFor(options);
    benchmark_support::printRunCsvHeader(std::cout);
    const std::vector<RunMetrics> measured = benchmark_support::runProfile(
        options.common, [&options](RunPhase phase, std::size_t runIndex) {
          return runOnce(options, phase, runIndex);
        });
    for (std::size_t index = 0; index < measured.size(); ++index) {
      benchmark_support::printRunCsv(std::cout, metadata, RunPhase::Measured, index + 1,
                                     measured[index]);
    }

    quill::Backend::stop();
    backendStarted = false;
    std::cout << "\n# summary rows\n";
    benchmark_support::printSummaryCsvHeader(std::cout);
    benchmark_support::printSummaryCsv(std::cout, metadata, measured);
    return 0;
  } catch (const std::exception& error) {
    if (backendStarted) {
      quill::Backend::stop();
    }
    std::cerr << "Quill benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
