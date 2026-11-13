// What the shader translator's sources reference from the rest of the
// emulator, cut down to what a command-line word counter needs. Part of the
// SPIR-V harness (README.md here), not of the app.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <utility>

#include "xenia/base/console.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/system.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/ui/renderdoc_api.h"

// Defined in files the harness does not build, with their defaults there
// (texture_cache.cc, command_processor.cc, render_target_cache.cc).
DEFINE_bool(texture_integer_num_format, false, "See texture_cache.cc.", "GPU");
DEFINE_bool(precise_interpolation, true, "See command_processor.cc.", "GPU");
DEFINE_bool(draw_resolution_scaled_texture_offsets, true,
            "See render_target_cache.cc.", "GPU");

namespace xe {

namespace logging {

bool ShouldLog(LogLevel log_level, uint32_t log_mask) {
  return log_level <= LogLevel::Info;
}

namespace internal {

static thread_local char thread_log_buffer[64 * 1024];

uint32_t GetLogLevel() { return uint32_t(LogLevel::Info); }

std::pair<char*, size_t> GetThreadBuffer() {
  return {thread_log_buffer, sizeof(thread_log_buffer)};
}

void AppendLogLine(LogLevel log_level, const char prefix_char,
                   size_t written) {
  std::fprintf(stderr, "%c> %.*s\n", prefix_char, int(written),
               thread_log_buffer);
}

}  // namespace internal
}  // namespace logging

bool has_console_attached() { return true; }
void AttachConsole() {}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  std::fprintf(stderr, "%.*s\n", int(message.size()), message.data());
}

#if XE_ARCH_AMD64
namespace amd64 {
uint64_t GetFeatureFlags() { return 0; }
}  // namespace amd64
#endif

namespace memory {
bool IsWritableExecutableMemorySupported() { return false; }
}  // namespace memory

namespace ui {
std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
  return nullptr;
}
RenderDocAPI::~RenderDocAPI() = default;
}  // namespace ui

namespace gpu {
namespace draw_util {
// As in draw_util.cc.
const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
const int8_t kD3D10StandardSamplePositions4x[4][2] = {
    {-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
}  // namespace draw_util
}  // namespace gpu

}  // namespace xe
