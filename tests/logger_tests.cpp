#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "BlockingQueue.h"
#include "Logger.h"

namespace {
namespace fs = std::filesystem;

class TestFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// 轻量断言：失败时带上源码位置，避免为本项目额外引入测试框架依赖。
#define CHECK(condition)                                                         \
  do {                                                                           \
    if (!(condition)) {                                                          \
      throw TestFailure(std::string(__FILE__) + ':' + std::to_string(__LINE__) + \
                        ": check failed: " #condition);                          \
    }                                                                            \
  } while (false)

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(const std::string& testName) {
    // 每个测试使用独立目录，避免日志文件相互影响，也避免污染工作区。
    static std::atomic_uint sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = fs::temp_directory_path() / "cpp_logger_tests" /
            (testName + "_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    fs::create_directories(path_);
  }

  ~TemporaryDirectory() {
    // 测试完成后清理临时日志；失败时也由栈展开触发清理。
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const noexcept {
    return path_;
  }

 private:
  fs::path path_;
};

std::vector<fs::path> logFiles(const fs::path& directory) {
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (entry.is_regular_file() && entry.path().extension() == ".log") {
      files.push_back(entry.path());
    }
  }
  // 使滚动文件的验证顺序稳定。
  std::sort(files.begin(), files.end());
  return files;
}

std::string readFile(const fs::path& filename) {
  std::ifstream input(filename, std::ios::binary);
  CHECK(input.is_open());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string readAllLogs(const fs::path& directory) {
  std::string contents;
  for (const auto& filename : logFiles(directory)) {
    contents += readFile(filename);
  }
  return contents;
}

std::size_t countOccurrences(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

void testBlockingQueueLifecycle() {
  BlockingQueue<std::string> queue;

  CHECK(queue.push("first") == QueuePushResult::Enqueued);
  CHECK(queue.push("second") == QueuePushResult::Enqueued);
  CHECK(queue.size() == 2);
  CHECK(!queue.empty());

  std::vector<std::string> batch;
  CHECK(queue.popBatch(batch, 1));
  CHECK(batch.size() == 1);
  CHECK(batch.front() == "first");
  CHECK(queue.size() == 1);

  queue.close();
  // close() 后应先取完已有元素，再通知消费者退出。
  CHECK(queue.closed());
  CHECK(queue.push("rejected") == QueuePushResult::Closed);

  const auto second = queue.pop();
  CHECK(second.has_value());
  CHECK(*second == "second");
  CHECK(!queue.popBatch(batch, 2));
  CHECK(batch.empty());

  queue.reset();
  CHECK(!queue.closed());
  CHECK(queue.empty());
  CHECK(queue.push("after-reset") == QueuePushResult::Enqueued);
  const auto afterReset = queue.pop();
  CHECK(afterReset.has_value());
  CHECK(*afterReset == "after-reset");

  CHECK(!queue.popBatchFor(batch, 1, std::chrono::milliseconds(1)));
  CHECK(queue.push("timed-pop") == QueuePushResult::Enqueued);
  CHECK(queue.popBatchFor(batch, 1, std::chrono::milliseconds(1)));
  CHECK(batch.size() == 1);
  CHECK(batch.front() == "timed-pop");
}

void testQueueOverflowPoliciesAndStats() {
  BlockingQueue<int> queue;

  queue.reset(2, OverflowPolicy::DropNewest);
  CHECK(queue.push(1) == QueuePushResult::Enqueued);
  CHECK(queue.push(2) == QueuePushResult::Enqueued);
  CHECK(queue.push(3) == QueuePushResult::DroppedNewest);
  CHECK(queue.size() == 2);
  CHECK(queue.peakSize() == 2);
  CHECK(queue.droppedCount() == 1);
  CHECK(*queue.pop() == 1);
  CHECK(*queue.pop() == 2);

  queue.reset(2, OverflowPolicy::DropOldest);
  CHECK(queue.push(1) == QueuePushResult::Enqueued);
  CHECK(queue.push(2) == QueuePushResult::Enqueued);
  CHECK(queue.push(3) == QueuePushResult::DroppedOldest);
  CHECK(queue.size() == 2);
  CHECK(queue.peakSize() == 2);
  CHECK(queue.droppedCount() == 1);
  CHECK(*queue.pop() == 2);
  CHECK(*queue.pop() == 3);

  queue.reset(1, OverflowPolicy::Block);
  const std::size_t generation = queue.generation();
  CHECK(queue.push(generation, 1) == QueuePushResult::Enqueued);

  // 队列满时，第二个生产者必须等待消费者 pop() 释放容量，且不能产生丢弃计数。
  std::promise<void> producerStarted;
  std::future<void> started = producerStarted.get_future();
  std::promise<void> producerFinished;
  std::future<void> finished = producerFinished.get_future();
  QueuePushResult producerResult = QueuePushResult::Closed;
  std::thread producer([&] {
    producerStarted.set_value();
    producerResult = queue.push(generation, 2);
    producerFinished.set_value();
  });

  started.get();
  const bool producerWasBlocked =
      finished.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
  const auto first = queue.pop();
  const bool producerWasReleased =
      finished.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  if (!producerWasReleased) {
    // 若实现回归导致未唤醒，主动关闭队列以避免失败测试永久阻塞在 join()。
    queue.close();
  }
  producer.join();

  CHECK(producerWasBlocked);
  CHECK(producerWasReleased);
  CHECK(first.has_value());
  CHECK(*first == 1);
  CHECK(producerResult == QueuePushResult::Enqueued);
  CHECK(*queue.pop() == 2);
  CHECK(queue.peakSize() == 1);
  CHECK(queue.droppedCount() == 0);

  // close() 也必须唤醒 Block 中的生产者，否则 Logger::stop() 可能无法完成。
  queue.reset(1, OverflowPolicy::Block);
  const std::size_t closingGeneration = queue.generation();
  CHECK(queue.push(closingGeneration, 1) == QueuePushResult::Enqueued);
  std::promise<void> closingProducerStarted;
  std::future<void> closingStarted = closingProducerStarted.get_future();
  std::promise<void> closingProducerFinished;
  std::future<void> closingFinished = closingProducerFinished.get_future();
  QueuePushResult closingResult = QueuePushResult::Enqueued;
  std::thread closingProducer([&] {
    closingProducerStarted.set_value();
    closingResult = queue.push(closingGeneration, 2);
    closingProducerFinished.set_value();
  });

  closingStarted.get();
  const bool closingProducerWasBlocked =
      closingFinished.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout;
  queue.close();
  const bool closingProducerWasReleased =
      closingFinished.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  closingProducer.join();

  CHECK(closingProducerWasBlocked);
  CHECK(closingProducerWasReleased);
  CHECK(closingResult == QueuePushResult::Closed);
  CHECK(queue.droppedCount() == 0);
}

void testInitializationValidationAndDirectoryCreation() {
  auto& logger = Logger::instance();
  logger.stop();

  CHECK(!logger.init({}, LogLevel::DEBUG));
  CHECK(!logger.init("ignored", LogLevel::DEBUG, 0));

  TemporaryDirectory temporaryDirectory("initialization");
  const fs::path logBasePath = temporaryDirectory.path() / "nested" / "app";
  LoggerConfig config;
  config.basePath = logBasePath;
  config.minLevel = LogLevel::INFO;
  config.queueCapacity = 8;
  config.overflowPolicy = OverflowPolicy::DropNewest;
  CHECK(logger.init(config));
  CHECK(fs::is_directory(logBasePath.parent_path()));
  CHECK(logger.queueSize() == 0);
  CHECK(logger.queuePeakSize() == 0);
  CHECK(logger.droppedCount() == 0);
  CHECK(!logger.init(temporaryDirectory.path() / "other", LogLevel::INFO));
  logger.stop();

  config.queueCapacity = 0;
  CHECK(!logger.init(config));

  config.queueCapacity = 8;
  config.writeBatchSize = 0;
  CHECK(!logger.init(config));

  config.writeBatchSize = 8;
  config.flushPolicy = FlushPolicy::Periodic;
  config.flushInterval = std::chrono::milliseconds::zero();
  CHECK(!logger.init(config));
}

void testPeriodicFlushAndFileOwnership() {
  auto& logger = Logger::instance();
  logger.stop();

  TemporaryDirectory temporaryDirectory("periodic_flush");
  LoggerConfig config;
  config.basePath = temporaryDirectory.path() / "app";
  config.writeBatchSize = 8;
  config.flushPolicy = FlushPolicy::Periodic;
  config.flushInterval = std::chrono::milliseconds(30);
  config.flushAtOrAbove = std::nullopt;
  CHECK(logger.init(config));

  std::string temporaryFile = "temporary-source.cpp";
  logger.log(LogLevel::INFO, temporaryFile.c_str(), 42, "periodic-flush-message");
  temporaryFile.assign("changed-after-log.cpp");

  const bool becameVisible = waitUntil(
      [&] {
        return readAllLogs(temporaryDirectory.path()).find("periodic-flush-message") !=
               std::string::npos;
      },
      std::chrono::seconds(1));
  logger.stop();

  const std::string contents = readAllLogs(temporaryDirectory.path());
  CHECK(becameVisible);
  CHECK(contents.find("[temporary-source.cpp:42]") != std::string::npos);
  CHECK(contents.find("changed-after-log.cpp") == std::string::npos);
}

void testBinaryOutputUsesByteAccurateNewlines() {
  auto& logger = Logger::instance();
  logger.stop();

  TemporaryDirectory temporaryDirectory("binary_output");
  LoggerConfig config;
  config.basePath = temporaryDirectory.path() / "app";
  config.flushPolicy = FlushPolicy::OnStop;
  config.flushAtOrAbove = std::nullopt;
  CHECK(logger.init(config));
  logger.log(LogLevel::INFO, "byte_test.cpp", 1, "byte-accurate-message");
  logger.stop();

  const std::vector<fs::path> files = logFiles(temporaryDirectory.path());
  CHECK(files.size() == 1);
  const std::string contents = readFile(files.front());
  CHECK(contents.size() >= 2);
  CHECK(contents.back() == '\n');
  CHECK(contents[contents.size() - 2] != '\r');
}

void testLevelTriggeredFlush() {
  auto& logger = Logger::instance();
  logger.stop();

  TemporaryDirectory temporaryDirectory("level_flush");
  LoggerConfig config;
  config.basePath = temporaryDirectory.path() / "app";
  config.flushPolicy = FlushPolicy::Periodic;
  config.flushInterval = std::chrono::seconds(10);
  config.flushAtOrAbove = LogLevel::ERROR;
  CHECK(logger.init(config));
  logger.log(LogLevel::ERROR, "level_flush.cpp", 7, "error-triggered-flush-message");

  const bool becameVisible = waitUntil(
      [&] {
        return readAllLogs(temporaryDirectory.path()).find("error-triggered-flush-message") !=
               std::string::npos;
      },
      std::chrono::seconds(1));
  logger.stop();

  CHECK(becameVisible);
}

void testLevelFilteringAndRuntimeLevelChange() {
  auto& logger = Logger::instance();
  logger.stop();

  TemporaryDirectory temporaryDirectory("level_filtering");
  const fs::path logBasePath = temporaryDirectory.path() / "app";
  CHECK(logger.init(logBasePath, LogLevel::WARN));

  CHECK(!logger.shouldLog(LogLevel::DEBUG));
  CHECK(!logger.shouldLog(LogLevel::INFO));
  CHECK(logger.shouldLog(LogLevel::WARN));
  LOG_DEBUG("filtered-debug-message");
  LOG_INFO("filtered-info-message");
  LOG_WARN("kept-warn-message");

  logger.setLevel(LogLevel::ERROR);
  CHECK(!logger.shouldLog(LogLevel::WARN));
  CHECK(logger.shouldLog(LogLevel::ERROR));
  LOG_WARN("filtered-warn-message");
  LOG_ERROR("kept-error-message");
  logger.stop();

  const std::string contents = readAllLogs(temporaryDirectory.path());
  // 格式化已移至后台线程，仍需保留级别和线程标识等字段。
  CHECK(contents.find("[WARN]") != std::string::npos);
  CHECK(contents.find("[ERROR]") != std::string::npos);
  CHECK(contents.find("[tid:") != std::string::npos);
  CHECK(contents.find("kept-warn-message") != std::string::npos);
  CHECK(contents.find("kept-error-message") != std::string::npos);
  CHECK(contents.find("filtered-debug-message") == std::string::npos);
  CHECK(contents.find("filtered-info-message") == std::string::npos);
  CHECK(contents.find("filtered-warn-message") == std::string::npos);
}

void testStopDrainsQueuedMessagesAndRejectsLaterMessages() {
  auto& logger = Logger::instance();
  logger.stop();

  TemporaryDirectory temporaryDirectory("drain_on_stop");
  CHECK(logger.init(temporaryDirectory.path() / "app"));

  constexpr std::size_t kMessageCount = 128;
  for (std::size_t index = 0; index < kMessageCount; ++index) {
    LOG_INFO("drain-message-" + std::to_string(index));
  }
  // stop() 会关闭队列并等待后台线程写完已经入队的全部消息。
  logger.stop();

  CHECK(!logger.shouldLog(LogLevel::INFO));
  LOG_INFO("message-after-stop");

  const std::string contents = readAllLogs(temporaryDirectory.path());
  for (std::size_t index = 0; index < kMessageCount; ++index) {
    CHECK(contents.find("drain-message-" + std::to_string(index)) != std::string::npos);
  }
  CHECK(contents.find("message-after-stop") == std::string::npos);
}

void testSizeBasedRotation() {
  auto& logger = Logger::instance();
  logger.stop();

  TemporaryDirectory temporaryDirectory("size_rotation");
  // 将阈值设为 1 B：第一条写入空文件，后续每条都会触发一次滚动。
  CHECK(logger.init(temporaryDirectory.path() / "app", LogLevel::DEBUG, 1));
  LOG_INFO("rotation-message-one");
  LOG_INFO("rotation-message-two");
  LOG_INFO("rotation-message-three");
  logger.stop();

  const std::vector<fs::path> files = logFiles(temporaryDirectory.path());
  CHECK(files.size() == 3);
  CHECK(readFile(files[0]).find("rotation-message-one") != std::string::npos);
  CHECK(readFile(files[1]).find("rotation-message-two") != std::string::npos);
  CHECK(readFile(files[2]).find("rotation-message-three") != std::string::npos);
}

void testReinitializationAndConcurrentLogging() {
  auto& logger = Logger::instance();
  logger.stop();

  TemporaryDirectory temporaryDirectory("reinitialization");
  const fs::path firstPath = temporaryDirectory.path() / "first" / "app";
  const fs::path secondPath = temporaryDirectory.path() / "second" / "app";

  CHECK(logger.init(firstPath));
  LOG_INFO("first-run-message");
  logger.stop();

  CHECK(logger.init(secondPath));
  constexpr std::size_t kThreadCount = 4;
  constexpr std::size_t kMessagesPerThread = 50;
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);
  for (std::size_t threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
    workers.emplace_back([threadIndex] {
      for (std::size_t messageIndex = 0; messageIndex < kMessagesPerThread; ++messageIndex) {
        LOG_INFO("concurrent-message-" + std::to_string(threadIndex) + '-' +
                 std::to_string(messageIndex));
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  // 所有生产者退出后再停止日志器，确保检查的是完整的异步写入结果。
  logger.stop();

  const std::string firstContents = readAllLogs(firstPath.parent_path());
  const std::string secondContents = readAllLogs(secondPath.parent_path());
  CHECK(firstContents.find("first-run-message") != std::string::npos);
  CHECK(secondContents.find("first-run-message") == std::string::npos);
  CHECK(countOccurrences(secondContents, "concurrent-message-") ==
        kThreadCount * kMessagesPerThread);
}

int runTest(const std::string& name, const std::function<void()>& test) {
  try {
    test();
    std::cout << "[PASS] " << name << '\n';
    return 0;
  } catch (const std::exception& error) {
    // Logger 是单例；失败时也停止它，防止后续测试继承运行状态。
    Logger::instance().stop();
    std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    return 1;
  }
}
}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"BlockingQueue lifecycle", testBlockingQueueLifecycle},
      {"queue overflow policies and statistics", testQueueOverflowPoliciesAndStats},
      {"initialization validation and directory creation",
       testInitializationValidationAndDirectoryCreation},
      {"periodic flush and file ownership", testPeriodicFlushAndFileOwnership},
      {"binary output uses byte-accurate newlines", testBinaryOutputUsesByteAccurateNewlines},
      {"level-triggered flush", testLevelTriggeredFlush},
      {"level filtering and runtime level change", testLevelFilteringAndRuntimeLevelChange},
      {"stop drains queued messages", testStopDrainsQueuedMessagesAndRejectsLaterMessages},
      {"size-based rotation", testSizeBasedRotation},
      {"reinitialization and concurrent logging", testReinitializationAndConcurrentLogging},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    failures += runTest(name, test);
  }
  return failures == 0 ? 0 : 1;
}
