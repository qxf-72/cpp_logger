#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BenchmarkHarness.h"
#include "Logger.h"

namespace {

using benchmark_support::BenchmarkMetadata;
using benchmark_support::Clock;
using benchmark_support::CommonOptions;
using benchmark_support::RunContext;
using benchmark_support::RunMetrics;
using benchmark_support::RunPhase;

struct BenchmarkOptions {
  CommonOptions common;
  // cpp_logger 的队列按“记录数”计量；总容量随生产者数量线性增长。
  std::size_t queueCapacityPerProducer{2048};
  std::size_t writeBatchSize{LoggerConfig::kDefaultWriteBatchSize};
};

struct BenchmarkProfile {
  std::string_view id;
  std::string_view queuePolicy;
  OverflowPolicy overflowPolicy;
};

void printUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n\n"
            << "Measure cpp_logger asynchronous file logging.\n\n"
            << "Common options:\n";
  benchmark_support::printCommonUsage(std::cout);
  std::cout << "\ncpp_logger options:\n"
            << "  --queue-per-thread <N>    Queue records per producer (default: 2048)\n"
            << "  --batch-size <N>          Records formatted and written per backend batch "
               "(default: 256)\n"
            << "  --help                    Show this help message\n\n"
            << "Profiles: block, drop-newest, drop-oldest\n";
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
    if (benchmark_support::parseCommonValueOption(options.common, argument, value)) {
      continue;
    }
    if (argument == "--queue-per-thread") {
      options.queueCapacityPerProducer = benchmark_support::parsePositiveSize(value, argument);
    } else if (argument == "--batch-size") {
      options.writeBatchSize = benchmark_support::parsePositiveSize(value, argument);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }

  benchmark_support::validateCommonOptions(options.common);
  if (options.common.threadCount >
      std::numeric_limits<std::size_t>::max() / options.queueCapacityPerProducer) {
    throw std::invalid_argument("threads multiplied by queue capacity is too large");
  }
  return options;
}

std::size_t queueCapacity(const BenchmarkOptions& options) {
  return options.common.threadCount * options.queueCapacityPerProducer;
}

void validateProfiles(const BenchmarkOptions& options,
                      const std::vector<BenchmarkProfile>& profiles) {
  for (const std::string& selectedProfile : options.common.selectedProfiles) {
    const bool found = std::any_of(profiles.begin(), profiles.end(),
                                   [&selectedProfile](const BenchmarkProfile& profile) {
                                     return profile.id == selectedProfile;
                                   });
    if (!found) {
      throw std::invalid_argument("unknown --profile value: " + selectedProfile);
    }
  }
}

FlushPolicy loggerFlushPolicy(benchmark_support::FlushMode mode) {
  return mode == benchmark_support::FlushMode::FinalDrain ? FlushPolicy::OnStop
                                                          : FlushPolicy::Periodic;
}

RunMetrics runOnce(const BenchmarkOptions& options, const BenchmarkProfile& profile, RunPhase phase,
                   std::size_t runIndex) {
  RunContext context =
      benchmark_support::makeRunContext(options.common, "cpp_logger", profile.id, phase, runIndex);
  Logger& logger = Logger::instance();
  bool initialized = false;

  try {
    // 先确保上一轮已经结束，避免单例残留状态污染本轮配置。
    logger.stop();

    LoggerConfig config;
    config.basePath = context.outputDirectory / "cpp_logger";
    config.minLevel = LogLevel::INFO;
    // 基准不测滚动行为，使用实际不可达到的阈值保持单文件写入路径稳定。
    config.maxFileSize = std::numeric_limits<std::size_t>::max() / 2;
    config.queueCapacity = queueCapacity(options);
    config.overflowPolicy = profile.overflowPolicy;
    config.writeBatchSize = options.writeBatchSize;
    config.flushPolicy = loggerFlushPolicy(options.common.flushMode);
    config.flushInterval = std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(options.common.flushIntervalMilliseconds));
    config.flushAtOrAbove = std::nullopt;
    config.enableConsoleSink = false;

    if (!logger.init(config)) {
      throw std::runtime_error("unable to initialize cpp_logger");
    }
    initialized = true;

    auto producers = benchmark_support::startProducers(
        options.common, context.payload, [](const std::string& message) {
          // 使用对外公开的日志调用路径；正文在后台线程格式化。
          Logger::instance().logStatic(LogLevel::INFO, __FILE__, __LINE__, message);
        });
    const benchmark_support::ProducerTiming timing = producers->timing();

    // stop() 关闭输入、排空已经接收的记录并刷新文件，因此结束时间是端到端完成边界。
    logger.stop();
    initialized = false;
    const Clock::time_point finished = Clock::now();
    const std::uint64_t dropped = logger.droppedCount();

    // 所有库都在排空后才回收生产者，避免线程局部资源提前销毁影响后台消费。
    producers->releaseAndJoin();
    const RunMetrics metrics = benchmark_support::finalizeRun(context, timing, finished, dropped);
    benchmark_support::cleanupRunDirectory(context, options.common.keepLogs);
    return metrics;
  } catch (...) {
    if (initialized) {
      logger.stop();
    }
    benchmark_support::cleanupRunDirectory(context, options.common.keepLogs);
    throw;
  }
}

BenchmarkMetadata metadataFor(const BenchmarkOptions& options, const BenchmarkProfile& profile) {
  return {"cpp_logger",
          "0.1.1",
          std::string(profile.id),
          std::string(profile.queuePolicy),
          std::to_string(queueCapacity(options)) + " records total",
          std::string(benchmark_support::flushModeName(options.common.flushMode))};
}

void printPreamble(const BenchmarkOptions& options) {
  std::cout << "# benchmark=cpp_logger\n"
            << "# threads=" << options.common.threadCount
            << ", messages_per_thread=" << options.common.messagesPerThread
            << ", payload_bytes=" << options.common.payloadSize
            << ", warmups=" << options.common.warmupRuns
            << ", measured_runs=" << options.common.measuredRuns
            << ", queue_per_thread=" << options.queueCapacityPerProducer
            << ", batch_size=" << options.writeBatchSize << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    const BenchmarkOptions options = parseOptions(argc, argv);
    const std::vector<BenchmarkProfile> profiles = {
        {"block", "Block", OverflowPolicy::Block},
        {"drop-newest", "DropNewest", OverflowPolicy::DropNewest},
        {"drop-oldest", "DropOldest", OverflowPolicy::DropOldest},
    };
    validateProfiles(options, profiles);

    printPreamble(options);
    benchmark_support::printRunCsvHeader(std::cout);

    std::vector<std::pair<BenchmarkMetadata, std::vector<RunMetrics>>> allResults;
    for (const BenchmarkProfile& profile : profiles) {
      if (!benchmark_support::shouldRunProfile(options.common, profile.id)) {
        continue;
      }

      const BenchmarkMetadata metadata = metadataFor(options, profile);
      const std::vector<RunMetrics> measured = benchmark_support::runProfile(
          options.common, [&options, &profile](RunPhase phase, std::size_t runIndex) {
            return runOnce(options, profile, phase, runIndex);
          });

      for (std::size_t index = 0; index < measured.size(); ++index) {
        benchmark_support::printRunCsv(std::cout, metadata, RunPhase::Measured, index + 1,
                                       measured[index]);
      }
      allResults.emplace_back(metadata, measured);
    }

    std::cout << "\n# summary rows\n";
    benchmark_support::printSummaryCsvHeader(std::cout);
    for (const auto& result : allResults) {
      benchmark_support::printSummaryCsv(std::cout, result.first, result.second);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "cpp_logger benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
