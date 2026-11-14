// hakuX-Boosted bench: stands in for the NDK header on Linux.
// base/host_thread_state.cc asks for the device's API level before it uses
// bionic's TLS_SLOT_APP (free for apps from level 29). On the bench the slot
// is the launcher's own word (core/launcher.c), so any level from 29 will do.
#ifndef BOOSTED_BENCH_COMPAT_ANDROID_API_LEVEL_H_
#define BOOSTED_BENCH_COMPAT_ANDROID_API_LEVEL_H_
static inline int android_get_device_api_level(void) { return 35; }
#endif
