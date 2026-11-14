// hakuX-Boosted benchmark rig: timing and result output shared by every
// benchmark program. Header only, no dependencies beyond libc.
//
// A result is one JSON object on one line, appended to the file named by the
// BENCH_OUT environment variable (or printed to stdout without it):
//   {"section":"prim","name":"mutex_lock_unlock","config":"","metric":
//    "ns_per_op","value":12.3}
// section/name/config/metric identify a number; tools/report.py renders the
// table and the difference against another run from these lines.
#ifndef BOOSTED_BENCH_COMMON_BENCH_H_
#define BOOSTED_BENCH_COMMON_BENCH_H_

#include <sched.h>
#include <time.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace bench {

inline uint64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// Keeps a value alive without the compiler seeing through it.
template <typename T>
inline void Keep(T&& value) {
  __asm__ __volatile__("" : : "r,m"(value) : "memory");
}

// Stay on one core for the length of a measurement: a migration between
// cores costs more than most of what is measured here.
inline void PinToCurrentCpu() {
  int cpu = sched_getcpu();
  if (cpu < 0) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof(set), &set);
}

inline void Unpin() {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i = 0; i < CPU_SETSIZE && i < 1024; ++i) {
    CPU_SET(i, &set);
  }
  sched_setaffinity(0, sizeof(set), &set);
}

inline uint64_t SampleNs() {
  const char* env = std::getenv("BENCH_SAMPLE_MS");
  uint64_t ms = env ? std::strtoull(env, nullptr, 10) : 20;
  return (ms ? ms : 20) * 1000000ull;
}

inline int SampleCount() {
  const char* env = std::getenv("BENCH_SAMPLES");
  int n = env ? std::atoi(env) : 9;
  return n > 0 ? n : 9;
}

struct Timing {
  double min_ns = 0;     // the fastest sample, per operation
  double median_ns = 0;  // the middle sample, per operation
  uint64_t batch = 0;    // operations per sample
};

// Times |op|, called |batch| times a sample with the batch sized so that a
// sample lasts about SampleNs(). The minimum is the number to compare (noise
// on a shared runner only ever adds time); the median says how noisy it was.
template <typename Op>
inline Timing Measure(Op&& op) {
  const uint64_t target = SampleNs();
  uint64_t batch = 1;
  for (;;) {
    uint64_t t0 = NowNs();
    for (uint64_t i = 0; i < batch; ++i) {
      op();
    }
    uint64_t dt = NowNs() - t0;
    if (dt >= target / 4 || batch >= (1ull << 40)) {
      if (dt > 0) {
        batch = std::max<uint64_t>(1, uint64_t(double(batch) * target / dt));
      }
      break;
    }
    batch *= 4;
  }
  std::vector<double> samples;
  const int count = SampleCount();
  for (int s = 0; s < count; ++s) {
    uint64_t t0 = NowNs();
    for (uint64_t i = 0; i < batch; ++i) {
      op();
    }
    uint64_t dt = NowNs() - t0;
    samples.push_back(double(dt) / double(batch));
  }
  std::sort(samples.begin(), samples.end());
  Timing timing;
  timing.min_ns = samples.front();
  timing.median_ns = samples[samples.size() / 2];
  timing.batch = batch;
  return timing;
}

inline std::string JsonEscape(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

inline void Emit(const std::string& section, const std::string& name,
                 const std::string& config, const std::string& metric,
                 double value) {
  const char* path = std::getenv("BENCH_OUT");
  FILE* file = path ? std::fopen(path, "a") : stdout;
  if (!file) {
    file = stdout;
  }
  std::fprintf(file,
               "{\"section\":\"%s\",\"name\":\"%s\",\"config\":\"%s\","
               "\"metric\":\"%s\",\"value\":%.6g}\n",
               JsonEscape(section).c_str(), JsonEscape(name).c_str(),
               JsonEscape(config).c_str(), JsonEscape(metric).c_str(), value);
  if (file != stdout) {
    std::fclose(file);
  }
  // A readable copy on stderr, so a log of the run can be read on its own.
  std::fprintf(stderr, "  %-8s %-44s %-28s %-18s %12.4g\n", section.c_str(),
               name.c_str(), config.c_str(), metric.c_str(), value);
}

// A text fact about the run (CPU model, which TLS resolver was bound).
inline void EmitNote(const std::string& section, const std::string& name,
                     const std::string& text) {
  const char* path = std::getenv("BENCH_OUT");
  FILE* file = path ? std::fopen(path, "a") : stdout;
  if (!file) {
    file = stdout;
  }
  std::fprintf(file, "{\"section\":\"%s\",\"name\":\"%s\",\"note\":\"%s\"}\n",
               JsonEscape(section).c_str(), JsonEscape(name).c_str(),
               JsonEscape(text).c_str());
  if (file != stdout) {
    std::fclose(file);
  }
  std::fprintf(stderr, "  %-8s %-44s %s\n", section.c_str(), name.c_str(),
               text.c_str());
}

inline void EmitTiming(const std::string& section, const std::string& name,
                       const std::string& config, const Timing& timing) {
  Emit(section, name, config, "ns_per_op", timing.min_ns);
  Emit(section, name, config, "ns_per_op_median", timing.median_ns);
}

}  // namespace bench

#endif  // BOOSTED_BENCH_COMMON_BENCH_H_
