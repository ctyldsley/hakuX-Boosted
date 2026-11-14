// hakuX-Boosted bench: entry point of the benchmarks built on the core's
// own sources. One suite and one set of settings per process, because the
// core reads several of its settings once, at start-up.
//
//   bench_core --suite=fiber|sched|xlat [--label=TEXT] [--set name=value]...
//
// Settings are the core's own configuration variables, looked up by name in
// its registry. A name this tree does not have is reported and the process
// exits with status 3 without measuring, so a benchmark never silently runs
// with a setting it was asked for missing.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"

#include "bench.h"

namespace bench {
int RunXlatSuite(const std::string& label);
int RunSchedSuite(const std::string& label, void* slot_address);
int RunFiberSuite(const std::string& label, void* slot_address);
}  // namespace bench

// Larger than the loader's spare static TLS, so that when this library is
// loaded with dlopen its thread-locals are reached through the dynamic
// resolver, as bionic reaches those of libe.so.
static thread_local char g_tls_pad[65536];

extern "C" __attribute__((visibility("default"))) int bench_core_main(
    int argc, char** argv, void* slot_address) {
  bench::Keep(g_tls_pad[0]);
  std::string suite, label;
  std::vector<std::string> settings;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--suite=", 0) == 0) {
      suite = arg.substr(8);
    } else if (arg.rfind("--label=", 0) == 0) {
      label = arg.substr(8);
    } else if (arg == "--set" && i + 1 < argc) {
      settings.push_back(argv[++i]);
    } else {
      std::fprintf(stderr, "bench_core: unknown argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (label.empty()) {
    label = "defaults";
  }

  std::vector<std::string> launch = {"bench_core"};
  for (const std::string& setting : settings) {
    auto equals = setting.find('=');
    std::string name = setting.substr(0, equals);
    if (!cvar::ConfigVars || !cvar::ConfigVars->count(name)) {
      bench::EmitNote(suite, "(" + label + ")",
                      "NOT MEASURED: this tree has no setting called " + name);
      return 3;
    }
    launch.push_back("--" + setting);
  }
  std::vector<char*> launch_argv;
  for (std::string& item : launch) {
    launch_argv.push_back(item.data());
  }
  launch_argv.push_back(nullptr);
  int launch_argc = int(launch.size());
  char** launch_ptr = launch_argv.data();
  cvar::ParseLaunchArguments(launch_argc, launch_ptr, "", {});

  int status;
  if (suite == "xlat") {
    status = bench::RunXlatSuite(label);
  } else if (suite == "sched") {
    status = bench::RunSchedSuite(label, slot_address);
  } else if (suite == "fiber") {
    status = bench::RunFiberSuite(label, slot_address);
  } else {
    std::fprintf(stderr, "bench_core: --suite=fiber, sched or xlat\n");
    return 2;
  }
  std::fflush(nullptr);
  // The core's objects are not torn down: nothing here needs them to be,
  // and the kernel stand-in has no destructors to run.
  _exit(status);
}
