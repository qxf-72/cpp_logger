#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <spdlog/version.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

constexpr std::chrono::seconds kPeriodicFlushInterval{1};

struct BenchmarkOptions {
  std::size_t threadCount{4};
  std::size_t messagesPerThread{250000};
  std::size_t payloadSize{128};
  std::size_t runs{3};
  // spdlog 的异步线程池使用全局 MPMC 队列；该值按生产者数线性放大，
  // 与 cpp_logger 对照行的总记录容量保持一致。
  std::size_t queueCapacityPerProducer{2048};
  fs::path outputDirectory{"comparison_benchmark_logs"};
  bool keepLogs{false};
  std::vector<std::string> selectedProfiles;
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

struct BenchmarkProfile {
  std::string_view id;
  std::string_view scenario;
  std::string_view queuePolicy;
  spdlog::async_overflow_policy overflowPolicy;
};

void printUsage(const char* program) {
  std::cout << "Usage: " << program << " [options]\n\n"
            << "This program measures spdlog asynchronous file logging only.\n\n"
            << "Options:\n"
            << "  --threads <N>          Producer thread count (default: 4)\n"
            << "  --messages <N>         Messages per producer (default: 250000)\n"
            << "  --payload <N>          Message payload size in bytes (default: 128)\n"
            << "  --runs <N>             Repeated runs per profile (default: 3)\n"
            << "  --queue-per-thread <N> Async queue records per producer (default: 2048)\n"
            << "  --output <PATH>        Temporary output directory\n"
            << "  --keep-logs            Keep log files after each run\n"
            << "  --profile <ID>         reliable_periodic, discard_new, or overrun_oldest "
               "(repeatable)\n"
            << "  --help                 Show this help message\n";
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
    } else if (argument == "--queue-per-thread") {
      options.queueCapacityPerProducer = parsePositiveSize(value, argument);
    } else if (argument == "--output") {
      options.outputDirectory = std::string(value);
    } else if (argument == "--profile") {
      options.selectedProfiles.emplace_back(value);
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }

  if (options.threadCount > std::numeric_limits<std::size_t>::max() / options.messagesPerThread) {
    throw std::invalid_argument("threads multiplied by messages is too large");
  }
  if (options.threadCount >
      std::numeric_limits<std::size_t>::max() / options.queueCapacityPerProducer) {
    throw std::invalid_argument("threads multiplied by queue capacity is too large");
  }
  return options;
}

std::size_t queueCapacity(const BenchmarkOptions& options) {
  return options.threadCount * options.queueCapacityPerProducer;
}

fs::path makeRunDirectory(const BenchmarkOptions& options, const BenchmarkProfile& profile,
                          std::size_t runIndex) {
  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      options.outputDirectory /
      (std::string(profile.id) + "_spdlog_run_" + std::to_string(runIndex) + "_" +
       std::to_string(timestamp));
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

RunResult runSpdlog(const BenchmarkOptions& options, const BenchmarkProfile& profile,
                    std::size_t runIndex) {
  const fs::path directory = makeRunDirectory(options, profile, runIndex);
  const std::string loggerName = "spdlog_benchmark_" + std::string(profile.id) + "_" +
                                 std::to_string(runIndex) + "_" + directory.string();

  // spdlog 的线程池和 registry 是进程级全局资源。每轮重新创建并关闭它们，
  // 确保前一轮的异步队列、定时 flush 线程不会影响下一轮。
  spdlog::shutdown();
  try {
    spdlog::init_thread_pool(queueCapacity(options), 1);
    auto threadPool = spdlog::thread_pool();
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_st>(
        (directory / "spdlog.log").string(), true);
    auto logger = std::make_shared<spdlog::async_logger>(loggerName, sink, threadPool,
                                                         profile.overflowPolicy);
    logger->set_level(spdlog::level::info);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l][tid:%t][%s:%#] %v");
    logger->flush_on(spdlog::level::off);
    spdlog::register_logger(logger);
    spdlog::flush_every(kPeriodicFlushInterval);

    // 源位置在同一轮的每一条记录中都相同；提前构造，避免让该无关对象进入生产者热路径。
    const spdlog::source_loc source{__FILE__, __LINE__, SPDLOG_FUNCTION};
    const auto startTime = runProducers(options, [&logger, source](const std::string& message) {
      logger->log(source, spdlog::level::info,
                  spdlog::string_view_t(message.data(), message.size()));
    });
    const auto producersDone = std::chrono::steady_clock::now();
    const auto dropped = static_cast<std::uint64_t>(
        profile.overflowPolicy == spdlog::async_overflow_policy::overrun_oldest
            ? threadPool->overrun_counter()
            : threadPool->discard_counter());

    // DiscardNew 队列已满时连 flush 请求都可能被拒绝，不能将 logger->flush()
    // 作为排空边界。下面先从 registry 移除 logger，再销毁本地持有的线程池；
    // thread_pool 析构会在队尾插入终止消息并 join 后端线程，保证此前接收的
    // 记录都已处理。最后显式刷新仍由局部变量持有的 sink。
    spdlog::drop(loggerName);
    logger.reset();
    spdlog::shutdown();
    threadPool.reset();
    sink->flush();
    const auto finished = std::chrono::steady_clock::now();
    sink.reset();
    removeRunDirectory(directory, options.keepLogs);
    return {std::chrono::duration<double>(producersDone - startTime).count(),
            std::chrono::duration<double>(finished - startTime).count(), dropped};
  } catch (...) {
    spdlog::drop(loggerName);
    spdlog::shutdown();
    removeRunDirectory(directory, options.keepLogs);
    throw;
  }
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

bool shouldRunProfile(const BenchmarkOptions& options, std::string_view profileId) {
  return options.selectedProfiles.empty() ||
         std::find(options.selectedProfiles.begin(), options.selectedProfiles.end(), profileId) !=
             options.selectedProfiles.end();
}

void printResult(const BenchmarkOptions& options, const BenchmarkProfile& profile,
                 const AggregateResult& result) {
  const std::size_t attempted = options.threadCount * options.messagesPerThread;
  const double accepted = std::max(0.0, static_cast<double>(attempted) - result.dropped);
  const double producerRate = static_cast<double>(attempted) / result.producerSeconds;
  const double endToEndRate = accepted / result.endToEndSeconds;
  const double droppedRate = result.dropped * 100.0 / static_cast<double>(attempted);

  std::cout << "| " << profile.scenario << " | spdlog | " << profile.queuePolicy << " | "
            << queueCapacity(options) << " records total | Periodic 1 s | " << std::fixed
            << std::setprecision(0) << producerRate << " | " << endToEndRate << " | "
            << std::setprecision(4) << droppedRate << "% |\n"
            << std::flush;
}
}  // namespace

int main(int argc, char* argv[]) {
  try {
    const BenchmarkOptions options = parseOptions(argc, argv);
    const std::vector<BenchmarkProfile> profiles = {
        {"reliable_periodic", "Reliable / periodic", "Block",
         spdlog::async_overflow_policy::block},
        // v1.17.0 的 discard_new 与 cpp_logger 的 DropNewest 都是队列满时拒绝
        // 当前新记录，因此可以用于“容忍丢失 / 丢新”的语义对照。
        {"discard_new", "Loss-tolerant / discard newest", "DiscardNew",
         spdlog::async_overflow_policy::discard_new},
        // 此模式覆盖最旧的待写记录，属于“保留最新日志”的独立场景；目前没有
        // 与其他两库完全对齐的行，故不放入 README 的横向表格。
        {"overrun_oldest", "Loss-tolerant / overrun oldest", "OverrunOldest",
         spdlog::async_overflow_policy::overrun_oldest},
    };

    for (const std::string& selectedProfile : options.selectedProfiles) {
      const bool found = std::any_of(profiles.begin(), profiles.end(),
                                     [&selectedProfile](const BenchmarkProfile& profile) {
                                       return profile.id == selectedProfile;
                                     });
      if (!found) {
        throw std::invalid_argument("unknown --profile value: " + selectedProfile);
      }
    }

    std::cout << "# spdlog asynchronous file benchmark\n"
              << "# spdlog_version=" << SPDLOG_VER_MAJOR << '.' << SPDLOG_VER_MINOR << '.'
              << SPDLOG_VER_PATCH << ", threads=" << options.threadCount
              << ", messages_per_thread=" << options.messagesPerThread
              << ", payload_bytes=" << options.payloadSize << ", runs=" << options.runs << '\n'
              << "# queue=" << queueCapacity(options)
              << " records total, worker_threads=1, flush=Periodic 1 s\n"
              << "# Producer throughput counts all attempted submissions; end-to-end throughput "
                 "counts records retained after overflow handling and completely flushed to the file.\n\n"
              << "| Scenario | Implementation | Queue policy | Queue capacity | Flush policy | "
                 "Producer logs/s | End-to-end logs/s | Drop rate |\n"
              << "| --- | --- | --- | --- | --- | ---: | ---: | ---: |\n"
              << std::flush;

    for (const BenchmarkProfile& profile : profiles) {
      if (!shouldRunProfile(options, profile.id)) {
        continue;
      }
      const AggregateResult result =
          runRepeated(options, [&](std::size_t runIndex) { return runSpdlog(options, profile, runIndex); });
      printResult(options, profile, result);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "spdlog benchmark failed: " << error.what() << '\n';
    spdlog::shutdown();
    return 1;
  }
}
