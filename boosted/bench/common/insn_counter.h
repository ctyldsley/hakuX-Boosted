// hakuX-Boosted bench: counts the instructions this thread retires in user
// mode between Start() and Stop(), through the kernel's perf events. Needs a
// CPU performance counter the kernel exposes, which a virtual machine may not
// have: ok() says whether it does, and why_not() what the kernel answered.
#ifndef BOOSTED_BENCH_COMMON_INSN_COUNTER_H_
#define BOOSTED_BENCH_COMMON_INSN_COUNTER_H_

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace bench {

class InsnCounter {
 public:
  InsnCounter() {
    perf_event_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    fd_ = int(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
    if (fd_ < 0) {
      why_not_ = std::string("perf_event_open: ") + std::strerror(errno);
      return;
    }
    // A counter that opens but does not count is no use either.
    Start();
    volatile uint64_t sink = 0;
    for (int i = 0; i < 1000; ++i) {
      sink = sink + i;
    }
    if (Stop() < 1000) {
      why_not_ = "the counter opened but did not count";
      close(fd_);
      fd_ = -1;
    }
  }
  ~InsnCounter() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  InsnCounter(const InsnCounter&) = delete;
  InsnCounter& operator=(const InsnCounter&) = delete;

  bool ok() const { return fd_ >= 0; }
  const std::string& why_not() const { return why_not_; }

  void Start() {
    ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
  }
  uint64_t Stop() {
    ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t count = 0;
    if (read(fd_, &count, sizeof(count)) != sizeof(count)) {
      return 0;
    }
    return count;
  }

 private:
  int fd_ = -1;
  std::string why_not_;
};

}  // namespace bench

#endif  // BOOSTED_BENCH_COMMON_INSN_COUNTER_H_
