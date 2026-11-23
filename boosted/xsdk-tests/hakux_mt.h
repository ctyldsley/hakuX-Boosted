// hakuX-Boosted: what the multi-threaded test titles share. Built inside xsdk's tree (copied to tests/), with xsdk's
// compiler flags, by hakux.mk. Each title logs PASS:/FAIL: lines, a summary, and returns its failure count, so xsdk's
// exit logs "exit: status N". A watchdog thread ends a title that overruns its time limit, logging the step it was in.
#ifndef HAKUX_MT_H
#define HAKUX_MT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	HAKUX_STACK_BYTES = 64 * 1024,
	HAKUX_LINE_BYTES = 128,
	HAKUX_WATCHDOG_HARDWARE_THREAD = 5,
};

// A word on a 128-byte cache line (the console's line and reservation granule) of its own.
typedef struct {
	volatile uint32_t value;
	uint8_t pad[HAKUX_LINE_BYTES - sizeof(uint32_t)];
} __attribute__((aligned(HAKUX_LINE_BYTES))) HakuxLine;

static const char *hakux_title __attribute__((unused));
static volatile const char *hakux_current_step __attribute__((unused)) = "start";
static unsigned hakux_passed __attribute__((unused));
static unsigned hakux_failed __attribute__((unused));
static int64_t hakux_deadline_ns __attribute__((unused));
static uint32_t hakux_limit_seconds __attribute__((unused));
static char hakux_step_text[160] __attribute__((unused));

static inline int64_t hakux_now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static inline void hakux_check(bool passed, const char *format, ...) __attribute__((format(printf, 2, 3)));
static inline void hakux_check(bool passed, const char *format, ...)
{
	char text[256];
	va_list args;
	va_start(args, format);
	(void)vsnprintf(text, sizeof text, format, args);
	va_end(args);
	xsdk_log_write("%s: %s: %s", passed ? "PASS" : "FAIL", hakux_title, text);
	hakux_passed += passed;
	hakux_failed += !passed;
}

// Logged before every blocking step, so a log that stops shows where.
static inline void hakux_step(const char *format, ...) __attribute__((format(printf, 1, 2)));
static inline void hakux_step(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	(void)vsnprintf(hakux_step_text, sizeof hakux_step_text, format, args);
	va_end(args);
	hakux_current_step = hakux_step_text;
	xsdk_log_write("%s: step: %s", hakux_title, hakux_step_text);
}

// Ends the title now with a failure, for when carrying on would be unsafe (a worker still running on our stack).
static inline void hakux_abort(const char *why) __attribute__((noreturn));
static inline void hakux_abort(const char *why)
{
	xsdk_log_write("FAIL: %s: aborted: %s (during: %s)", hakux_title, why, (const char *)hakux_current_step);
	hakux_failed++;
	xsdk_log_write("%s: %u passed, %u failed (aborted)", hakux_title, hakux_passed, hakux_failed);
	_exit((int)hakux_failed);
}

static inline uint32_t hakux_watchdog(void *context)
{
	(void)context;
	for (;;) {
		if (hakux_now_ns() >= hakux_deadline_ns) {
			xsdk_log_write("FAIL: %s: time limit of %u s reached during: %s", hakux_title,
				(unsigned)hakux_limit_seconds, (const char *)hakux_current_step);
			hakux_failed++;
			xsdk_log_write("%s: %u passed, %u failed (stopped by the time limit)", hakux_title, hakux_passed,
				hakux_failed);
			_exit((int)hakux_failed);
		}
		(void)xsdk_thread_sleep(100);
	}
	return 0;
}

static inline void hakux_begin(const char *title, const char *what, uint32_t limit_seconds)
{
	hakux_title = title;
	hakux_limit_seconds = limit_seconds;
	hakux_deadline_ns = hakux_now_ns() + (int64_t)limit_seconds * 1000000000;
	xsdk_log_write("%s: %s; time limit %u s", title, what, (unsigned)limit_seconds);
	const NTSTATUS moved = xsdk_thread_move(0);
	if (!NT_SUCCESS(moved))
		xsdk_log_write("%s: xsdk_thread_move(0) failed: 0x%08X", title, (unsigned)moved);
	HANDLE watchdog;
	const NTSTATUS status = xsdk_thread_create_on(
		&watchdog, HAKUX_STACK_BYTES, hakux_watchdog, NULL, HAKUX_WATCHDOG_HARDWARE_THREAD);
	if (!NT_SUCCESS(status)) {
		hakux_check(false, "watchdog thread starts (0x%08X)", (unsigned)status);
		return;
	}
	(void)xsdk_thread_set_priority(watchdog, XSDK_PRIORITY_MAX, NULL);
	(void)NtClose(watchdog);
}

static inline int hakux_end(void)
{
	hakux_current_step = "end";
	xsdk_log_write("%s: %u passed, %u failed", hakux_title, hakux_passed, hakux_failed);
	return (int)hakux_failed;
}

static inline NTSTATUS hakux_wait_ms(HANDLE handle, uint32_t ms)
{
	const int64_t timeout = -(int64_t)ms * 10000;
	return NtWaitForSingleObjectEx(handle, USER_MODE, 0, &timeout);
}

static inline NTSTATUS hakux_wait_many_ms(uint32_t count, const HANDLE *handles, uint32_t type, uint32_t ms)
{
	const int64_t timeout = -(int64_t)ms * 10000;
	return NtWaitForMultipleObjectsEx(count, handles, type, USER_MODE, 0, &timeout);
}

// Starts entry on a hardware thread; aborts the title if it cannot, as every check after would be meaningless.
static inline HANDLE hakux_spawn(XsdkThreadEntry entry, void *context, uint32_t hardware_thread)
{
	HANDLE thread;
	const NTSTATUS status = xsdk_thread_create_on(&thread, HAKUX_STACK_BYTES, entry, context, hardware_thread);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("%s: xsdk_thread_create_on(%u) failed: 0x%08X", hakux_title, (unsigned)hardware_thread,
			(unsigned)status);
		hakux_abort("a worker thread could not start");
	}
	return thread;
}

// Waits for a worker to end and closes it; a worker still running after ms may yet touch our stack, so that aborts.
static inline void hakux_join(HANDLE thread, uint32_t ms)
{
	const NTSTATUS status = hakux_wait_ms(thread, ms);
	if (status != STATUS_SUCCESS) {
		xsdk_log_write("%s: a worker did not end within %u ms: 0x%08X", hakux_title, (unsigned)ms, (unsigned)status);
		hakux_abort("a worker did not end");
	}
	(void)NtClose(thread);
}

static inline void hakux_compiler_barrier(void)
{
	__asm__ volatile("" : : : "memory");
}

static inline void hakux_lwsync(void)
{
	__asm__ volatile("lwsync" : : : "memory");
}

static inline void hakux_sync(void)
{
	__asm__ volatile("sync" : : : "memory");
}

#endif
