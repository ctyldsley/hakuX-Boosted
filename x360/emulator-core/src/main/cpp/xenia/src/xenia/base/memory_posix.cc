/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif  // XE_PLATFORM_MAC

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

// hakuX-Boosted: for the userfaultfd write watch below.
#if XE_PLATFORM_LINUX
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#if defined(__ANDROID__)
#include <android/api-level.h>
#endif
#endif

#if XE_PLATFORM_xendroid
#include <android/sharedmem.h>
#endif
// Both ANDROID and xendroid are defined on the fork; the allocation path below is
// exclusive (#if xendroid #elif ANDROID), but upstream's standalone dlopen setup
// block is bare #if ANDROID, so its includes must be available even on xendroid.
#if XE_PLATFORM_ANDROID
#include <dlfcn.h>
#include <linux/ashmem.h>
#include <string.h>
#include <sys/ioctl.h>

#include "xenia/base/main_android.h"
#endif

namespace xe {
namespace memory {

#if XE_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (xe::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ =
          reinterpret_cast<decltype(android_ASharedMemory_create_)>(
              dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

// hakuX-Boosted: asked once. getpagesize() was 3.8% of a guest thread that
// polls and yields (device profile, Fable II): the scheduler and the heaps
// ask on every pass, and bionic's is a function call into sysconf.
size_t page_size() {
  static const size_t size = static_cast<size_t>(getpagesize());
  return size;
}
size_t allocation_granularity() { return page_size(); }

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

PageAccess ToXeniaProtectFlags(const char* protection) {
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == 'x') {
    return PageAccess::kExecuteReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == 'x') {
    return PageAccess::kExecuteReadOnly;
  }
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == '-') {
    return PageAccess::kReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == '-') {
    return PageAccess::kReadOnly;
  }
  return PageAccess::kNoAccess;
}

#if XE_PLATFORM_MAC
bool IsWritableExecutableMemorySupported() {
  // macOS allows RWX only on anonymous MAP_JIT regions. Callers that see
  // true must allocate via AllocFixed (which sets MAP_JIT) and toggle
  // pthread_jit_write_protect_np around writes. MAP_JIT requires the
  // com.apple.security.cs.allow-jit entitlement; without it the probe
  // fails and JIT is disabled.
  static const bool supported = []() {
    const size_t test_size = page_size();
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    flags |= MAP_JIT;
#endif
    void* test_mapping = mmap(nullptr, test_size,
                              PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    if (test_mapping == MAP_FAILED) {
      XELOGE("MAP_JIT probe failed: {} ({}); JIT will not work",
             strerror(errno), errno);
      return false;
    }
    munmap(test_mapping, test_size);
    return true;
  }();
  return supported;
}
#else
bool IsWritableExecutableMemorySupported() { return true; }
#endif  // XE_PLATFORM_MAC

struct MappedFileRange {
  uintptr_t region_begin;
  uintptr_t region_end;
};

std::vector<MappedFileRange> mapped_file_ranges;
std::mutex g_mapped_file_ranges_mutex;

// Track shm file names for cleanup on exit
std::vector<std::string> g_shm_file_names;
std::mutex g_shm_file_names_mutex;
static bool g_cleanup_handlers_installed = false;

#if !XE_PLATFORM_ANDROID&& !XE_PLATFORM_xendroid
static void CleanupAtExit() {
  for (const auto& name : g_shm_file_names) {
    shm_unlink(name.c_str());
  }
}

static void InstallCleanupHandlers() {
  if (g_cleanup_handlers_installed) {
    return;
  }
  g_cleanup_handlers_installed = true;

  std::atexit(CleanupAtExit);
  std::at_quick_exit(CleanupAtExit);
}
#endif  // !XE_PLATFORM_ANDROID

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  // mmap does not support reserve / commit, so ignore allocation_type.
  uint32_t prot = ToPosixProtectFlags(access);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#if XE_PLATFORM_MAC
  if (access == PageAccess::kExecuteReadWrite ||
      access == PageAccess::kExecuteReadOnly) {
    flags |= MAP_JIT;
  }
#endif  // XE_PLATFORM_MAC

  if (base_address != nullptr) {
    if (allocation_type == AllocationType::kCommit) {
#if XE_PLATFORM_MAC
      size_t host_page = page_size();
      uintptr_t aligned_addr =
          reinterpret_cast<uintptr_t>(base_address) & ~(host_page - 1);
      uintptr_t end_addr = reinterpret_cast<uintptr_t>(base_address) + length;
      end_addr = (end_addr + host_page - 1) & ~(host_page - 1);
      if (mprotect(reinterpret_cast<void*>(aligned_addr),
                   end_addr - aligned_addr, prot) == 0) {
        return base_address;
      }
      return nullptr;
#else
      if (Protect(base_address, length, access)) {
        return base_address;
      }
      return nullptr;
#endif  // XE_PLATFORM_MAC
    }

    flags |= MAP_FIXED;
  }

  void* result = mmap(base_address, length, prot, flags, -1, 0);

  if (result == MAP_FAILED) {
    return nullptr;
  }

  if (base_address != nullptr && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  const auto region_begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t region_end =
      reinterpret_cast<uintptr_t>(base_address) + length;

  std::lock_guard guard(g_mapped_file_ranges_mutex);
  for (const auto& mapped_range : mapped_file_ranges) {
    if (region_begin >= mapped_range.region_begin &&
        region_end <= mapped_range.region_end) {
      switch (deallocation_type) {
        case DeallocationType::kDecommit:
          return Protect(base_address, length, PageAccess::kNoAccess);
        case DeallocationType::kRelease:
          return false;
        default:
          assert_unhandled_case(deallocation_type);
      }
    }
  }

  switch (deallocation_type) {
    case DeallocationType::kDecommit:
      return Protect(base_address, length, PageAccess::kNoAccess);
    case DeallocationType::kRelease:
      return munmap(base_address, length) == 0;
    default:
      assert_unhandled_case(deallocation_type);
  }
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) {
    size_t length_copy = length;
    QueryProtect(base_address, length_copy, *out_old_access);
  }

  uint32_t prot = ToPosixProtectFlags(access);
  int ret = mprotect(base_address, length, prot);
  if (ret != 0) {
    XELOGE("mprotect({}, 0x{:X}, {}) failed: {} ({})", base_address, length,
           prot, strerror(errno), errno);
  }
  // hakuX-Boosted: counted for the memory_watch_log line.
  if (watch_stats_enabled()) {
    WatchCounters& counters = watch_counters();
    counters.mprotect_calls.fetch_add(1, std::memory_order_relaxed);
    counters.mprotect_pages.fetch_add(
        (length + (page_size() - 1)) / page_size(), std::memory_order_relaxed);
  }
  return ret == 0;
}

// hakuX-Boosted: userfaultfd write-protection, see memory.h. Linux only (the
// Android build is XE_PLATFORM_LINUX); everywhere else it reports that it is
// not available and the caller keeps to mprotect.
#if XE_PLATFORM_LINUX
namespace {
// The descriptor, -1 until WriteWatchInitialize succeeds. Never closed: the
// registered ranges live as long as the process does.
int write_watch_fd_ = -1;

// Values from linux/userfaultfd.h that older NDK sysroots may lack. The
// kernel is asked at run time, so a header that knows less is no obstacle.
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFD_FEATURE_WP_HUGETLBFS_SHMEM
#define UFFD_FEATURE_WP_HUGETLBFS_SHMEM (1 << 12)
#endif
}  // namespace

bool WriteWatchInitialize() {
  if (write_watch_fd_ >= 0) {
    return true;
  }
#if defined(__ANDROID__)
  // Android's seccomp filter for apps kills the process on a system call it
  // does not list, and userfaultfd joined the list in Android 12 (API 31,
  // for ART's collector). Never make the call below that.
  if (android_get_device_api_level() < 31) {
    XELOGW(
        "Write watch: userfaultfd is not open to apps before Android 12 "
        "(this is API {}), watches stay on mprotect",
        android_get_device_api_level());
    return false;
  }
#endif
  // UFFD_USER_MODE_ONLY: Android sets vm.unprivileged_userfaultfd to 0, which
  // allows an app a descriptor that handles faults taken in user mode only.
  // That is all a watch needs: a write made by the kernel on the guest's
  // behalf into a protected page fails with EFAULT, as it does under mprotect.
  int fd = int(syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY));
  if (fd < 0) {
    XELOGW(
        "Write watch: userfaultfd is not available to this process ({}, {}), "
        "watches stay on mprotect",
        strerror(errno), errno);
    return false;
  }
  // Write-protect faults on shared memory, delivered as SIGBUS to the
  // faulting thread and not as messages on the descriptor: the handler must
  // run on the thread that wrote, before the write completes.
  struct uffdio_api api;
  std::memset(&api, 0, sizeof(api));
  api.api = UFFD_API;
  api.features = UFFD_FEATURE_SIGBUS | UFFD_FEATURE_PAGEFAULT_FLAG_WP |
                 UFFD_FEATURE_WP_HUGETLBFS_SHMEM;
  if (ioctl(fd, UFFDIO_API, &api) != 0) {
    int api_errno = errno;
    // Ask again for nothing, only to log what this kernel does offer.
    uint64_t offered = 0;
    int probe_fd =
        int(syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY));
    if (probe_fd >= 0) {
      struct uffdio_api probe;
      std::memset(&probe, 0, sizeof(probe));
      probe.api = UFFD_API;
      if (ioctl(probe_fd, UFFDIO_API, &probe) == 0) {
        offered = probe.features;
      }
      close(probe_fd);
    }
    XELOGW(
        "Write watch: this kernel's userfaultfd lacks write-protection of "
        "shared memory with SIGBUS delivery ({}, {}; features offered "
        "{:#x}, needed {:#x}), watches stay on mprotect",
        strerror(api_errno), api_errno, offered, uint64_t(api.features));
    close(fd);
    return false;
  }
  if (!(api.ioctls & (uint64_t(1) << _UFFDIO_REGISTER))) {
    XELOGW("Write watch: userfaultfd offers no register ioctl, watches stay "
           "on mprotect");
    close(fd);
    return false;
  }
  write_watch_fd_ = fd;
  XELOGI("Write watch: userfaultfd ready (features {:#x})",
         uint64_t(api.features));
  return true;
}

bool WriteWatchRegister(void* base_address, size_t length) {
  if (write_watch_fd_ < 0) {
    return false;
  }
  struct uffdio_register reg;
  std::memset(&reg, 0, sizeof(reg));
  reg.range.start = reinterpret_cast<uintptr_t>(base_address);
  reg.range.len = length;
  reg.mode = UFFDIO_REGISTER_MODE_WP;
  if (ioctl(write_watch_fd_, UFFDIO_REGISTER, &reg) != 0) {
    XELOGW(
        "Write watch: registering {} + {:#x} failed ({}, {}), watches there "
        "stay on mprotect",
        base_address, length, strerror(errno), errno);
    return false;
  }
  if (!(reg.ioctls & (uint64_t(1) << _UFFDIO_WRITEPROTECT))) {
    XELOGW(
        "Write watch: {} + {:#x} registered but the mapping cannot be "
        "write-protected (ioctls {:#x}), watches there stay on mprotect",
        base_address, length, uint64_t(reg.ioctls));
    struct uffdio_range range;
    range.start = reg.range.start;
    range.len = reg.range.len;
    ioctl(write_watch_fd_, UFFDIO_UNREGISTER, &range);
    return false;
  }
  return true;
}

bool WriteWatchProtect(void* base_address, size_t length, bool protect) {
  // Called from the fault handler: system calls and atomics only, and errno
  // is put back.
  if (write_watch_fd_ < 0) {
    return false;
  }
  int saved_errno = errno;
  struct uffdio_writeprotect wp;
  wp.range.start = reinterpret_cast<uintptr_t>(base_address);
  wp.range.len = length;
  wp.mode = protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0;
  int ret;
  int attempts = 0;
  do {
    ret = ioctl(write_watch_fd_, UFFDIO_WRITEPROTECT, &wp);
  } while (ret != 0 && (errno == EAGAIN || errno == EINTR) && ++attempts < 16);
  if (watch_stats_enabled()) {
    WatchCounters& counters = watch_counters();
    counters.uffd_calls.fetch_add(1, std::memory_order_relaxed);
    if (ret != 0) {
      counters.uffd_failures.fetch_add(1, std::memory_order_relaxed);
    }
  }
  errno = saved_errno;
  return ret == 0;
}
#else
bool WriteWatchInitialize() { return false; }
bool WriteWatchRegister(void* base_address, size_t length) { return false; }
bool WriteWatchProtect(void* base_address, size_t length, bool protect) {
  return false;
}
#endif  // XE_PLATFORM_LINUX

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if XE_PLATFORM_MAC
  mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name;

  kern_return_t kr = mach_vm_region(
      mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);

  if (kr != KERN_SUCCESS) {
    return false;
  }

  if (address > reinterpret_cast<mach_vm_address_t>(base_address)) {
    return false;
  }

  length =
      static_cast<size_t>((address + region_size) -
                          reinterpret_cast<mach_vm_address_t>(base_address));

  if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
      (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadWrite;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) ==
             (VM_PROT_READ | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadOnly;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) ==
             (VM_PROT_READ | VM_PROT_WRITE)) {
    access_out = PageAccess::kReadWrite;
  } else if (info.protection & VM_PROT_READ) {
    access_out = PageAccess::kReadOnly;
  } else {
    access_out = PageAccess::kNoAccess;
  }

  return true;
#else
  // No generic POSIX solution exists. The Linux solution should work on all
  // Linux kernel based OS, including Android.
  std::ifstream memory_maps;
  memory_maps.open("/proc/self/maps", std::ios_base::in);
  std::string maps_entry_string;

  while (std::getline(memory_maps, maps_entry_string)) {
    std::stringstream entry_stream(maps_entry_string);
    uintptr_t map_region_begin, map_region_end;
    char separator;
    char protection[5];  // 4 chars (e.g., "r-xp") + null terminator

    entry_stream >> std::hex >> map_region_begin >> separator >>
        map_region_end >> protection;

    if (map_region_begin <= reinterpret_cast<uintptr_t>(base_address) &&
        map_region_end > reinterpret_cast<uintptr_t>(base_address)) {
      length = map_region_end - reinterpret_cast<uintptr_t>(base_address);

      access_out = ToXeniaProtectFlags(protection);

      // Look at the next consecutive mappings
      while (std::getline(memory_maps, maps_entry_string)) {
        std::stringstream next_entry_stream(maps_entry_string);
        uintptr_t next_map_region_begin, next_map_region_end;
        char next_protection[5];  // 4 chars (e.g., "r-xp") + null terminator

        next_entry_stream >> std::hex >> next_map_region_begin >> separator >>
            next_map_region_end >> next_protection;
        if (map_region_end == next_map_region_begin &&
            access_out == ToXeniaProtectFlags(next_protection)) {
          length =
              next_map_region_end - reinterpret_cast<uintptr_t>(base_address);
          continue;
        }
        break;
      }

      memory_maps.close();
      return true;
    }
  }

  memory_maps.close();
  return false;
#endif  // XE_PLATFORM_MAC
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
#if XE_PLATFORM_xendroid
    //XELOGI("CreateFileMappingHandle: {} 0x{:X}", path.string(), length);
    int sharedmem_fd = ASharedMemory_create(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
#elif XE_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), xe::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return ashmem_fd;
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;

#if XE_PLATFORM_MAC
  std::string shm_name = "/" + path.filename().string();
  if (shm_name.size() > 30) {
    std::size_t h = std::hash<std::string>{}(shm_name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    shm_name = hash_buf;
  }
  int ret = shm_open(shm_name.c_str(), oflag, 0777);
  if (ret < 0) {
    XELOGE("shm_open({}) failed: {} ({})", shm_name, strerror(errno), errno);
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", shm_name, length,
           strerror(errno), errno);
    close(ret);
    shm_unlink(shm_name.c_str());
    return kFileMappingHandleInvalid;
  }
  // Track for cleanup on abnormal exit and install cleanup handlers
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    g_shm_file_names.push_back(shm_name);
  }
  InstallCleanupHandlers();
  return ret;
#else
  auto full_path = "/" / path;
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    XELOGE("shm_open({}) failed: {} ({})", full_path.string(), strerror(errno),
           errno);
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", full_path.string(), length,
           strerror(errno), errno);
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  // Track for cleanup on abnormal exit and install cleanup handlers
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    g_shm_file_names.push_back(full_path.string());
  }
  InstallCleanupHandlers();
  return ret;
#endif  // XE_PLATFORM_MAC
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  close(handle);
#if !XE_PLATFORM_ANDROID && !XE_PLATFORM_xendroid
#if XE_PLATFORM_MAC
  std::string shm_name = "/" + path.filename().string();
  if (shm_name.size() > 30) {
    std::size_t h = std::hash<std::string>{}(shm_name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    shm_name = hash_buf;
  }
  shm_unlink(shm_name.c_str());
  // Remove from tracking
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    auto it =
        std::find(g_shm_file_names.begin(), g_shm_file_names.end(), shm_name);
    if (it != g_shm_file_names.end()) {
      g_shm_file_names.erase(it);
    }
  }
#else
  auto full_path = "/" / path;
  shm_unlink(full_path.c_str());
  // Remove from tracking
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    auto it = std::find(g_shm_file_names.begin(), g_shm_file_names.end(),
                        full_path.string());
    if (it != g_shm_file_names.end()) {
      g_shm_file_names.erase(it);
    }
  }
#endif  // XE_PLATFORM_MAC
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  uint32_t prot = ToPosixProtectFlags(access);

  int flags = MAP_SHARED;
  if (base_address != nullptr) {
      flags |= MAP_FIXED;
  }

    void* result = mmap(base_address, length, prot, flags, handle, file_offset);

  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Without MAP_FIXED_NOREPLACE (e.g. macOS), a non-null base_address is just
  // a hint. Enforce the caller's contract by failing on address mismatch so
  // callers can retry at a different base, matching AllocFixed's behavior.
  if (base_address != nullptr && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  std::lock_guard guard(g_mapped_file_ranges_mutex);
  mapped_file_ranges.push_back({reinterpret_cast<uintptr_t>(result),
                                reinterpret_cast<uintptr_t>(result) + length});
  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  std::lock_guard guard(g_mapped_file_ranges_mutex);

#if XE_PLATFORM_MAC
  uintptr_t unmap_begin = reinterpret_cast<uintptr_t>(base_address);
  uintptr_t unmap_end = unmap_begin + length;

  for (auto mapped_range = mapped_file_ranges.begin();
       mapped_range != mapped_file_ranges.end(); ++mapped_range) {
    if (unmap_begin >= mapped_range->region_begin &&
        unmap_end <= mapped_range->region_end) {
      uintptr_t orig_begin = mapped_range->region_begin;
      uintptr_t orig_end = mapped_range->region_end;
      mapped_file_ranges.erase(mapped_range);

      if (orig_begin < unmap_begin) {
        mapped_file_ranges.push_back({orig_begin, unmap_begin});
      }
      if (unmap_end < orig_end) {
        mapped_file_ranges.push_back({unmap_end, orig_end});
      }

      return munmap(base_address, length) == 0;
    }
  }

  return munmap(base_address, length) == 0;
#else
  for (auto mapped_range = mapped_file_ranges.begin();
       mapped_range != mapped_file_ranges.end();) {
    if (mapped_range->region_begin ==
            reinterpret_cast<uintptr_t>(base_address) &&
        mapped_range->region_end ==
            reinterpret_cast<uintptr_t>(base_address) + length) {
      mapped_file_ranges.erase(mapped_range);
      return munmap(base_address, length) == 0;
    }
    ++mapped_range;
  }
  // TODO: Implement partial file unmapping.
  assert_always("Error: Partial unmapping of files not yet supported.");
  return munmap(base_address, length) == 0;
#endif  // XE_PLATFORM_MAC
}

}  // namespace memory
}  // namespace xe
