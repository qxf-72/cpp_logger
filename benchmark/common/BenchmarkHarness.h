#ifndef BENCHMARK_HARNESS_H
#define BENCHMARK_HARNESS_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace benchmark_support {

using Clock = std::chrono::steady_clock;

// 三个实现都可表达的刷新语义。FinalDrain 表示仅在每轮结束时等待排空并显式刷新。
enum class FlushMode { FinalDrain, Periodic };

// 所有 benchmark 共享的命令行参数，保证三套程序的工作负载和统计口径一致。
struct CommonOptions {
  std::size_t threadCount{4};
  std::size_t messagesPerThread{250000};
  std::size_t payloadSize{128};
  std::size_t warmupRuns{1};
  std::size_t measuredRuns{10};
  FlushMode flushMode{FlushMode::FinalDrain};
  std::size_t flushIntervalMilliseconds{1000};
  std::filesystem::path outputDirectory{"benchmark_logs"};
  bool keepLogs{false};
  std::vector<std::string> selectedProfiles;
};

enum class RunPhase { Warmup, Measured };

// 每轮生成唯一 marker，并将其嵌入固定长度正文；停止后通过 marker 计数校验实际落盘条数。
struct RunContext {
  std::filesystem::path outputDirectory;
  std::string marker;
  std::string payload;
  std::uint64_t attempted{0};
};

struct ProducerTiming {
  Clock::time_point started;
  Clock::time_point producersDone;
};

struct OutputStats {
  std::uint64_t deliveredRecords{0};
  std::uint64_t bytes{0};
  std::size_t fileCount{0};
};

struct RunMetrics {
  std::uint64_t attempted{0};
  std::uint64_t delivered{0};
  std::uint64_t dropped{0};
  OutputStats output;
  double producerSeconds{0.0};
  double drainSeconds{0.0};
  double endToEndSeconds{0.0};
};

struct Distribution {
  double minimum{0.0};
  double median{0.0};
  double maximum{0.0};
};

struct Summary {
  Distribution attemptedProducerRate;
  Distribution acceptedProducerRate;
  Distribution deliveredEndToEndRate;
  Distribution dropRatePercent;
  Distribution drainSeconds;
};

struct BenchmarkMetadata {
  std::string library;
  std::string libraryVersion;
  std::string profile;
  std::string queuePolicy;
  std::string queueCapacity;
  std::string flushMode;
};

std::size_t parsePositiveSize(std::string_view value, std::string_view option);
std::size_t parseNonNegativeSize(std::string_view value, std::string_view option);
FlushMode parseFlushMode(std::string_view value);
std::string_view flushModeName(FlushMode mode) noexcept;
std::string_view runPhaseName(RunPhase phase) noexcept;

// 返回 true 表示参数已由公共 harness 消费；库特有参数由各自 benchmark 继续解析。
bool parseCommonValueOption(CommonOptions& options, std::string_view argument,
                            std::string_view value);
void validateCommonOptions(const CommonOptions& options);
void printCommonUsage(std::ostream& output);
bool shouldRunProfile(const CommonOptions& options, std::string_view profile);

RunContext makeRunContext(const CommonOptions& options, std::string_view library,
                          std::string_view profile, RunPhase phase, std::size_t runIndex);
OutputStats inspectLogOutput(const RunContext& context);
void cleanupRunDirectory(const RunContext& context, bool keepLogs) noexcept;

RunMetrics finalizeRun(const RunContext& context, ProducerTiming timing, Clock::time_point finished,
                       std::optional<std::uint64_t> expectedDropped = std::nullopt);
double attemptedProducerRate(const RunMetrics& metrics) noexcept;
double acceptedProducerRate(const RunMetrics& metrics) noexcept;
double deliveredEndToEndRate(const RunMetrics& metrics) noexcept;
double dropRatePercent(const RunMetrics& metrics) noexcept;
double bytesPerDeliveredRecord(const RunMetrics& metrics) noexcept;
Summary summarize(const std::vector<RunMetrics>& metrics);

void printRunCsvHeader(std::ostream& output);
void printRunCsv(std::ostream& output, const BenchmarkMetadata& metadata, RunPhase phase,
                 std::size_t runIndex, const RunMetrics& metrics);
void printSummaryCsvHeader(std::ostream& output);
void printSummaryCsv(std::ostream& output, const BenchmarkMetadata& metadata,
                     const std::vector<RunMetrics>& metrics);

// 生产者在线程启动后先执行 prepare()，再进入统一起跑栅栏；因此线程局部队列预分配等工作
// 不会被计入 producer 吞吐。日志提交回调保持模板类型，避免 std::function 进入热路径。
template <typename Submit, typename Prepare>
class ProducerGroup final {
 public:
  static std::unique_ptr<ProducerGroup> start(const CommonOptions& options, std::string payload,
                                              Submit submit, Prepare prepare) {
    auto group = std::unique_ptr<ProducerGroup>(
        new ProducerGroup(options, std::move(payload), std::move(submit), std::move(prepare)));
    group->launchAndWaitForCompletion();
    return group;
  }

  ProducerGroup(const ProducerGroup&) = delete;
  ProducerGroup& operator=(const ProducerGroup&) = delete;

  ~ProducerGroup() {
    releaseAndJoin();
  }

  [[nodiscard]] ProducerTiming timing() const noexcept {
    return {started_, producersDone_};
  }

  // 日志库完成 drain 后再释放线程，可避免 Quill 的线程本地 SPSC 队列过早析构。
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
  ProducerGroup(const CommonOptions& options, std::string payload, Submit submit, Prepare prepare)
      : options_(options),
        payload_(std::move(payload)),
        submit_(std::move(submit)),
        prepare_(std::move(prepare)) {
    workers_.reserve(options_.threadCount);
  }

  void launchAndWaitForCompletion() {
    try {
      for (std::size_t index = 0; index < options_.threadCount; ++index) {
        workers_.emplace_back([this] { workerLoop(); });
      }
    } catch (...) {
      {
        std::scoped_lock lock(startMutex_);
        startWorkers_ = true;
      }
      startCondition_.notify_all();
      releaseAndJoin();
      throw;
    }

    {
      std::unique_lock lock(startMutex_);
      readyCondition_.wait(lock, [this] { return readyWorkers_ == options_.threadCount; });
      started_ = Clock::now();
      startWorkers_ = true;
    }
    startCondition_.notify_all();

    {
      std::unique_lock lock(completionMutex_);
      completionCondition_.wait(lock, [this] { return completedWorkers_ == options_.threadCount; });
      producersDone_ = Clock::now();
    }

    rethrowWorkerFailure();
  }

  void workerLoop() noexcept {
    bool canSubmit = true;
    try {
      prepare_();
    } catch (...) {
      recordWorkerFailure(std::current_exception());
      canSubmit = false;
    }

    {
      std::unique_lock lock(startMutex_);
      ++readyWorkers_;
      readyCondition_.notify_one();
      startCondition_.wait(lock, [this] { return startWorkers_; });
    }

    if (canSubmit) {
      try {
        for (std::size_t messageIndex = 0; messageIndex < options_.messagesPerThread;
             ++messageIndex) {
          submit_(payload_);
        }
      } catch (...) {
        recordWorkerFailure(std::current_exception());
      }
    }

    {
      std::scoped_lock lock(completionMutex_);
      ++completedWorkers_;
    }
    completionCondition_.notify_one();

    std::unique_lock lock(releaseMutex_);
    releaseCondition_.wait(lock, [this] { return releaseWorkers_; });
  }

  void recordWorkerFailure(std::exception_ptr failure) noexcept {
    std::scoped_lock lock(failureMutex_);
    if (workerFailure_ == nullptr) {
      workerFailure_ = failure;
    }
  }

  void rethrowWorkerFailure() {
    std::exception_ptr failure;
    {
      std::scoped_lock lock(failureMutex_);
      failure = workerFailure_;
    }
    if (failure != nullptr) {
      releaseAndJoin();
      std::rethrow_exception(failure);
    }
  }

  CommonOptions options_;
  std::string payload_;
  Submit submit_;
  Prepare prepare_;
  std::vector<std::thread> workers_;

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

  std::mutex failureMutex_;
  std::exception_ptr workerFailure_;
  Clock::time_point started_{};
  Clock::time_point producersDone_{};
};

struct NoopPrepare {
  void operator()() const noexcept {}
};

template <typename Submit, typename Prepare>
auto startProducers(const CommonOptions& options, const std::string& payload, Submit&& submit,
                    Prepare&& prepare) {
  using Group = ProducerGroup<std::decay_t<Submit>, std::decay_t<Prepare>>;
  return Group::start(options, payload, std::forward<Submit>(submit),
                      std::forward<Prepare>(prepare));
}

template <typename Submit>
auto startProducers(const CommonOptions& options, const std::string& payload, Submit&& submit) {
  return startProducers(options, payload, std::forward<Submit>(submit), NoopPrepare{});
}

template <typename RunOnce>
std::vector<RunMetrics> runProfile(const CommonOptions& options, RunOnce&& runOnce) {
  for (std::size_t runIndex = 1; runIndex <= options.warmupRuns; ++runIndex) {
    static_cast<void>(runOnce(RunPhase::Warmup, runIndex));
  }

  std::vector<RunMetrics> measured;
  measured.reserve(options.measuredRuns);
  for (std::size_t runIndex = 1; runIndex <= options.measuredRuns; ++runIndex) {
    measured.push_back(runOnce(RunPhase::Measured, runIndex));
  }
  return measured;
}

}  // namespace benchmark_support

#endif
