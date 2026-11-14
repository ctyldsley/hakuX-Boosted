package xendroid.compose.core

import android.os.Process
import android.os.SystemClock
import android.util.Log
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * hakuX-Boosted: ending the :emu process on purpose (boosted/notes/emu-lifecycle.md).
 *
 * The core's only pause (Emulator::Pause) is a soft one: it parks audio and the GPU and
 * leaves the guest threads running, on the reading that they will settle on their own
 * waits. A title that busy-waits (Fable II's 3D engine thread and its CPU 3 thread) does
 * not settle: it spins at a full core each, for as long as the process lives, with
 * nothing drawn. So a stopped host activity cannot be left alive indefinitely; it is
 * ended after [BACKGROUND_GRACE_MS], and quitting ends it at once.
 *
 * What "clean" means here: the guest's saves are written through pwrite as the guest
 * writes them (base/filesystem_posix.cc), so they are in the kernel's page cache and
 * survive the process ending; the config files are written whole to a .tmp and renamed
 * (config.cc, ConfigFiles.writeAtomic). What can be lost is an app-side write still in
 * flight on a background thread, so those are counted with [tracked] and the shutdown
 * waits for them, at most [WRITES_WAIT_MS]. The process then ends with SIGKILL, as it
 * always has (System.exit runs the C++ static destructors, which can deadlock joining a
 * paused audio worker). [startWatchdog] kills the process if any step hangs.
 */
object EmuShutdown {
    private const val TAG = "EmuShutdown"

    /** How long a stopped (home, another app, screen off) game is kept before it is ended. */
    const val BACKGROUND_GRACE_MS = 60_000L

    /** The whole shutdown, from its first step, must be over in this time or the process is killed. */
    const val WATCHDOG_MS = 5_000L

    /** The longest the shutdown waits for app-side config writes still in flight. */
    const val WRITES_WAIT_MS = 2_000L

    private val inFlight = AtomicInteger(0)
    private val lock = ReentrantLock()
    private val idle = lock.newCondition()
    private val watchdogStarted = AtomicBoolean(false)

    @PublishedApi internal fun begin() {
        inFlight.incrementAndGet()
    }

    @PublishedApi internal fun end() {
        if (inFlight.decrementAndGet() <= 0) lock.withLock { idle.signalAll() }
    }

    /** Runs a write of the config or the controls that the shutdown must not cut short. */
    inline fun <T> tracked(block: () -> T): T {
        begin()
        try {
            return block()
        } finally {
            end()
        }
    }

    /** True once no tracked write is in flight; false when [timeoutMs] ran out first. */
    fun awaitWrites(timeoutMs: Long): Boolean {
        val deadline = SystemClock.uptimeMillis() + timeoutMs
        lock.withLock {
            while (inFlight.get() > 0) {
                val left = deadline - SystemClock.uptimeMillis()
                if (left <= 0) return false
                idle.await(left, TimeUnit.MILLISECONDS)
            }
        }
        return true
    }

    /** Kills the process [WATCHDOG_MS] from now whatever happens; once per process. */
    fun startWatchdog(reason: String) {
        if (!watchdogStarted.compareAndSet(false, true)) return
        Thread({
            try {
                Thread.sleep(WATCHDOG_MS)
            } catch (e: InterruptedException) {
                // ends early: kill now
            }
            Log.w(TAG, "shutdown ($reason) not over after $WATCHDOG_MS ms; killing :emu")
            Process.killProcess(Process.myPid())
        }, "EmuShutdownWatchdog").apply {
            isDaemon = true
            start()
        }
    }

    /** The last step: ends the process, after the tracked writes (bounded). Off the main thread. */
    fun exitAfterWrites(reason: String) {
        Thread({
            if (!awaitWrites(WRITES_WAIT_MS)) {
                Log.w(TAG, "a config write was still in flight after $WRITES_WAIT_MS ms")
            }
            Log.i(TAG, "session ended ($reason); :emu exits")
            Process.killProcess(Process.myPid())
        }, "EmuShutdown").start()
    }
}
