#include "BenchmarkHarness.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>

namespace benchmark_support {
namespace {

std::size_t parseSize(std::string_view value, std::string_view option, bool allowZero) {
  unsigned long long parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || (!allowZero && parsed == 0) ||
      parsed > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("invalid value for " + std::string(option) + ": " +
                                std::string(value));
  }
  return static_cast<std::size_t>(parsed);
}

std::uint64_t checkedToUint64(std::size_t value, std::string_view description) {
  if (value > std::numeric_limits<std::uint64_t>::max()) {
    throw std::invalid_argument(std::string(description) + " is too large");
  }
  return static_cast<std::uint64_t>(value);
}

std::string csvEscape(std::string_view value) {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string(value);
  }

  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char character : value) {
    if (character == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(character);
  }
  escaped.push_back('"');
  return escaped;
}

Distribution makeDistribution(std::vector<double> values) {
  if (values.empty()) {
    throw std::invalid_argument("cannot summarize an empty benchmark result set");
  }

  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  const double median =
      values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
  return {values.front(), median, values.back()};
}

void printMetadata(std::ostream& output, const BenchmarkMetadata& metadata) {
  output << csvEscape(metadata.library) << ',' << csvEscape(metadata.libraryVersion) << ','
         << csvEscape(metadata.profile) << ',' << csvEscape(metadata.queuePolicy) << ','
         << csvEscape(metadata.queueCapacity) << ',' << csvEscape(metadata.flushMode);
}

void printDistribution(std::ostream& output, const Distribution& distribution) {
  output << std::fixed << std::setprecision(2) << distribution.minimum << ',' << distribution.median
         << ',' << distribution.maximum;
}

}  // namespace

std::size_t parsePositiveSize(std::string_view value, std::string_view option) {
  return parseSize(value, option, false);
}

std::size_t parseNonNegativeSize(std::string_view value, std::string_view option) {
  return parseSize(value, option, true);
}

FlushMode parseFlushMode(std::string_view value) {
  if (value == "final-drain") {
    return FlushMode::FinalDrain;
  }
  if (value == "periodic") {
    return FlushMode::Periodic;
  }
  throw std::invalid_argument("--flush-mode must be final-drain or periodic");
}

std::string_view flushModeName(FlushMode mode) noexcept {
  switch (mode) {
    case FlushMode::FinalDrain:
      return "final-drain";
    case FlushMode::Periodic:
      return "periodic";
  }
  return "unknown";
}

std::string_view runPhaseName(RunPhase phase) noexcept {
  switch (phase) {
    case RunPhase::Warmup:
      return "warmup";
    case RunPhase::Measured:
      return "measured";
  }
  return "unknown";
}

bool parseCommonValueOption(CommonOptions& options, std::string_view argument,
                            std::string_view value) {
  if (argument == "--threads") {
    options.threadCount = parsePositiveSize(value, argument);
  } else if (argument == "--messages") {
    options.messagesPerThread = parsePositiveSize(value, argument);
  } else if (argument == "--payload") {
    options.payloadSize = parsePositiveSize(value, argument);
  } else if (argument == "--warmups") {
    options.warmupRuns = parseNonNegativeSize(value, argument);
  } else if (argument == "--runs") {
    options.measuredRuns = parsePositiveSize(value, argument);
  } else if (argument == "--flush-mode") {
    options.flushMode = parseFlushMode(value);
  } else if (argument == "--flush-interval-ms") {
    options.flushIntervalMilliseconds = parsePositiveSize(value, argument);
  } else if (argument == "--output") {
    options.outputDirectory = std::string(value);
  } else if (argument == "--profile") {
    options.selectedProfiles.emplace_back(value);
  } else {
    return false;
  }
  return true;
}

void validateCommonOptions(const CommonOptions& options) {
  if (options.outputDirectory.empty()) {
    throw std::invalid_argument("--output must not be empty");
  }
  if (options.threadCount > std::numeric_limits<std::size_t>::max() / options.messagesPerThread) {
    throw std::invalid_argument("threads multiplied by messages is too large");
  }
  const std::size_t attempted = options.threadCount * options.messagesPerThread;
  static_cast<void>(checkedToUint64(attempted, "total message count"));
  if (options.flushIntervalMilliseconds >
      static_cast<std::size_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
    throw std::invalid_argument("flush interval is too large");
  }
}

void printCommonUsage(std::ostream& output) {
  output << "  --threads <N>              Producer thread count (default: 4)\n"
         << "  --messages <N>             Messages per producer (default: 250000)\n"
         << "  --payload <N>              Fixed message-body bytes, including validation marker "
            "(default: 128)\n"
         << "  --warmups <N>              Unreported complete warmup runs (default: 1)\n"
         << "  --runs <N>                 Measured runs per profile (default: 10)\n"
         << "  --flush-mode <X>           final-drain or periodic (default: final-drain)\n"
         << "  --flush-interval-ms <N>    Periodic flush interval (default: 1000)\n"
         << "  --output <PATH>            Root directory for temporary logs (default: "
            "benchmark_logs)\n"
         << "  --keep-logs                Retain verified log files after each run\n"
         << "  --profile <ID>             Run one profile; may be repeated\n";
}

bool shouldRunProfile(const CommonOptions& options, std::string_view profile) {
  return options.selectedProfiles.empty() ||
         std::find(options.selectedProfiles.begin(), options.selectedProfiles.end(), profile) !=
             options.selectedProfiles.end();
}

RunContext makeRunContext(const CommonOptions& options, std::string_view library,
                          std::string_view profile, RunPhase phase, std::size_t runIndex) {
  validateCommonOptions(options);

  static std::atomic<std::uint64_t> sequence{0};
  const std::uint64_t currentSequence = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto timestamp = Clock::now().time_since_epoch().count();
  const std::string phaseName(runPhaseName(phase));
  const std::string suffix = phaseName + "_" + std::to_string(runIndex) + "_" +
                             std::to_string(timestamp) + "_" + std::to_string(currentSequence);

  RunContext context;
  context.outputDirectory =
      options.outputDirectory / std::string(library) / std::string(profile) / suffix;
  context.marker =
      "[benchmark:" + std::string(library) + ":" + std::string(profile) + ":" + suffix + "]";
  if (context.marker.size() > options.payloadSize) {
    throw std::invalid_argument("--payload is too small for the benchmark validation marker");
  }
  context.payload = context.marker;
  context.payload.append(options.payloadSize - context.marker.size(), 'x');

  const std::size_t attempted = options.threadCount * options.messagesPerThread;
  context.attempted = checkedToUint64(attempted, "total message count");

  std::error_code error;
  std::filesystem::create_directories(context.outputDirectory, error);
  if (error) {
    throw std::runtime_error("unable to create benchmark directory: " + error.message());
  }
  return context;
}

OutputStats inspectLogOutput(const RunContext& context) {
  OutputStats stats;
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(context.outputDirectory, error);
  if (error) {
    throw std::runtime_error("unable to enumerate benchmark logs: " + error.message());
  }

  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    const std::filesystem::directory_entry entry = *iterator;
    iterator.increment(error);
    if (error) {
      throw std::runtime_error("unable to enumerate benchmark logs: " + error.message());
    }

    if (!entry.is_regular_file(error)) {
      if (error) {
        throw std::runtime_error("unable to inspect benchmark output: " + error.message());
      }
      continue;
    }
    if (entry.path().extension() != ".log") {
      continue;
    }

    const std::uintmax_t fileSize = entry.file_size(error);
    if (error) {
      throw std::runtime_error("unable to read benchmark output size: " + error.message());
    }
    if (fileSize > std::numeric_limits<std::uint64_t>::max() - stats.bytes) {
      throw std::runtime_error("benchmark output is too large to measure");
    }
    stats.bytes += static_cast<std::uint64_t>(fileSize);
    ++stats.fileCount;

    std::ifstream input(entry.path(), std::ios::binary);
    if (!input.is_open()) {
      throw std::runtime_error("unable to open benchmark output: " + entry.path().string());
    }

    std::string line;
    while (std::getline(input, line)) {
      std::size_t offset = 0;
      while ((offset = line.find(context.marker, offset)) != std::string::npos) {
        if (stats.deliveredRecords == std::numeric_limits<std::uint64_t>::max()) {
          throw std::runtime_error("too many validated benchmark records");
        }
        ++stats.deliveredRecords;
        offset += context.marker.size();
      }
    }
    if (!input.eof()) {
      throw std::runtime_error("unable to read benchmark output: " + entry.path().string());
    }
  }
  return stats;
}

void cleanupRunDirectory(const RunContext& context, bool keepLogs) noexcept {
  if (keepLogs) {
    return;
  }

  std::error_code error;
  std::filesystem::remove_all(context.outputDirectory, error);
  if (error) {
    std::cerr << "warning: unable to remove benchmark logs: " << error.message() << '\n';
  }
}

RunMetrics finalizeRun(const RunContext& context, ProducerTiming timing, Clock::time_point finished,
                       std::optional<std::uint64_t> expectedDropped) {
  RunMetrics metrics;
  metrics.attempted = context.attempted;
  metrics.output = inspectLogOutput(context);
  metrics.delivered = metrics.output.deliveredRecords;
  if (metrics.delivered > metrics.attempted) {
    throw std::runtime_error("validated output contains more records than were submitted");
  }
  metrics.dropped = metrics.attempted - metrics.delivered;
  if (expectedDropped.has_value() && *expectedDropped != metrics.dropped) {
    throw std::runtime_error("logger drop counter does not match validated output");
  }

  metrics.producerSeconds =
      std::chrono::duration<double>(timing.producersDone - timing.started).count();
  metrics.drainSeconds = std::chrono::duration<double>(finished - timing.producersDone).count();
  metrics.endToEndSeconds = std::chrono::duration<double>(finished - timing.started).count();
  if (metrics.producerSeconds <= 0.0 || metrics.drainSeconds < 0.0 ||
      metrics.endToEndSeconds <= 0.0) {
    throw std::runtime_error("invalid benchmark timing result");
  }
  return metrics;
}

double attemptedProducerRate(const RunMetrics& metrics) noexcept {
  return static_cast<double>(metrics.attempted) / metrics.producerSeconds;
}

double acceptedProducerRate(const RunMetrics& metrics) noexcept {
  return static_cast<double>(metrics.delivered) / metrics.producerSeconds;
}

double deliveredEndToEndRate(const RunMetrics& metrics) noexcept {
  return static_cast<double>(metrics.delivered) / metrics.endToEndSeconds;
}

double dropRatePercent(const RunMetrics& metrics) noexcept {
  return metrics.attempted == 0 ? 0.0
                                : static_cast<double>(metrics.dropped) * 100.0 /
                                      static_cast<double>(metrics.attempted);
}

double bytesPerDeliveredRecord(const RunMetrics& metrics) noexcept {
  return metrics.delivered == 0
             ? 0.0
             : static_cast<double>(metrics.output.bytes) / static_cast<double>(metrics.delivered);
}

Summary summarize(const std::vector<RunMetrics>& metrics) {
  std::vector<double> attemptedRates;
  std::vector<double> acceptedRates;
  std::vector<double> deliveredRates;
  std::vector<double> dropRates;
  std::vector<double> drainTimes;
  attemptedRates.reserve(metrics.size());
  acceptedRates.reserve(metrics.size());
  deliveredRates.reserve(metrics.size());
  dropRates.reserve(metrics.size());
  drainTimes.reserve(metrics.size());

  for (const RunMetrics& result : metrics) {
    attemptedRates.push_back(attemptedProducerRate(result));
    acceptedRates.push_back(acceptedProducerRate(result));
    deliveredRates.push_back(deliveredEndToEndRate(result));
    dropRates.push_back(dropRatePercent(result));
    drainTimes.push_back(result.drainSeconds);
  }

  return {makeDistribution(std::move(attemptedRates)), makeDistribution(std::move(acceptedRates)),
          makeDistribution(std::move(deliveredRates)), makeDistribution(std::move(dropRates)),
          makeDistribution(std::move(drainTimes))};
}

void printRunCsvHeader(std::ostream& output) {
  output
      << "record_type,library,library_version,profile,queue_policy,queue_capacity,flush_mode,phase,"
         "run,"
         "attempted,delivered,dropped,output_files,output_bytes,producer_seconds,drain_seconds,"
         "end_to_end_seconds,attempted_producer_logs_per_second,accepted_producer_logs_per_second,"
         "delivered_end_to_end_logs_per_second,drop_rate_percent,bytes_per_delivered_record\n";
}

void printRunCsv(std::ostream& output, const BenchmarkMetadata& metadata, RunPhase phase,
                 std::size_t runIndex, const RunMetrics& metrics) {
  output << "run,";
  printMetadata(output, metadata);
  output << ',' << runPhaseName(phase) << ',' << runIndex << ',' << metrics.attempted << ','
         << metrics.delivered << ',' << metrics.dropped << ',' << metrics.output.fileCount << ','
         << metrics.output.bytes << ',' << std::fixed << std::setprecision(6)
         << metrics.producerSeconds << ',' << metrics.drainSeconds << ',' << metrics.endToEndSeconds
         << ',' << std::setprecision(2) << attemptedProducerRate(metrics) << ','
         << acceptedProducerRate(metrics) << ',' << deliveredEndToEndRate(metrics) << ','
         << dropRatePercent(metrics) << ',' << bytesPerDeliveredRecord(metrics) << '\n';
}

void printSummaryCsvHeader(std::ostream& output) {
  output
      << "library,library_version,profile,queue_policy,queue_capacity,flush_mode,measured_runs,"
         "attempted_producer_rate_min,attempted_producer_rate_median,attempted_producer_rate_max,"
         "accepted_producer_rate_min,accepted_producer_rate_median,accepted_producer_rate_max,"
         "delivered_end_to_end_rate_min,delivered_end_to_end_rate_median,"
         "delivered_end_to_end_rate_max,drop_rate_percent_min,drop_rate_percent_median,"
         "drop_rate_percent_max,drain_seconds_min,drain_seconds_median,drain_seconds_max\n";
}

void printSummaryCsv(std::ostream& output, const BenchmarkMetadata& metadata,
                     const std::vector<RunMetrics>& metrics) {
  const Summary summary = summarize(metrics);
  printMetadata(output, metadata);
  output << ',' << metrics.size() << ',';
  printDistribution(output, summary.attemptedProducerRate);
  output << ',';
  printDistribution(output, summary.acceptedProducerRate);
  output << ',';
  printDistribution(output, summary.deliveredEndToEndRate);
  output << ',';
  printDistribution(output, summary.dropRatePercent);
  output << ',';
  printDistribution(output, summary.drainSeconds);
  output << '\n';
}

}  // namespace benchmark_support
