# The 360 core after the game is left: `claude/emu-lifecycle`

Branch `claude/emu-lifecycle`, cut from `claude/integration-1115`. Written
2026-11-14. Nothing here has run on a device: the build is the only check.

## What was seen

Twice on 2026-11-14 (after launch 1 and after run A, run 134's APK, Fable II),
with the owner out of the game and the launcher in front, the process
`com.rfandango.haku_x.debug:emu` was still alive, `Guest CPU 0` and
`Guest CPU 3` each at 96% of a core, no frames drawn. The phone heats until the
app is force-stopped. The owner says it happens for any title.

## What the code did, case by case (before this branch)

Line numbers are those of `claude/integration-1115` (`origin`, before this
branch's commit).

There is no foreground service and no wake lock anywhere on the 360 side
(searched `x360/app`, `boosted/android`, `android/app` for `startForeground`,
`WakeLock`, `onTaskRemoved`, `stopWithTask`: nothing). The only things that
keep a process up on purpose are:

- `FLAG_KEEP_SCREEN_ON` on the game's window
  (`x360/app/.../EmulatorHostActivity.kt:229`), which only holds the screen
  while the window is shown;
- `EmuProcessLink.bindToMainProcess` (`x360/app/.../core/EmuProcessLink.kt:54`):
  `:emu` binds `MainAliveService` in the main process with `BIND_IMPORTANT`.
  That raises the main process to `:emu`'s importance, not the other way, so it
  does not keep `:emu` alive. Its other half kills `:emu` when the main process
  dies while `:emu` is in the background (`:68`, `:73`).

Nothing ends the `:emu` process on purpose except `onDestroy` and that link.

| Event | Activity | Native core | Process |
|---|---|---|---|
| Paused (`onPause`, `EmulatorHostActivity.kt:572`) | `flushGpuCaches()`, which does nothing (the call is commented out in `xendroid_emu.cpp:825`) | runs | alive |
| Focus lost (shade, a dialog) | `pauseOnFocusLost` after 250 ms (`:122`, `:215`) | `Emulator::Pause` | alive |
| Stopped: home, another app, screen off (`onStop`, `:577`) | `session.pause()` | `Emulator::Pause` | **alive, indefinitely** |
| Surface destroyed (`surfaceDestroyed`, `:564`) | `session.pause()`, then `detachSurface()` (a synchronous GPU drain) | `Emulator::Pause`, swapchain torn down | alive |
| Started again (`onStart`, `:590`; `surfaceCreated`, `:550`) | re-attach, `resumeIfPaused()` unless the in-game menu is open | `Emulator::Resume` | alive |
| Quit in the in-game menu (`onQuit = { finish() }`, `:465`; the controller's `PAUSE_OPTION_QUIT -> finish()`, `:793`) | `finish()`, then `onDestroy` | prompts cancelled in `onDestroy` | `Process.killProcess` in `onDestroy` (`:600`) |
| Task swiped from recents | `onDestroy` if the system calls it; the system usually kills the process itself (`killProcessesForRemovedTask`) since `:emu` has no foreground service | as above | killed by the system or by `onDestroy` |

`Emulator::Pause` (`x360/emulator-core/.../xenia/emulator.cc:1426`) is a soft
pause by design: it pauses the audio system and the graphics system (command
processor, vblank) and leaves the guest threads running, on the comment's
reading that they "settle on their own waits"; suspending threads mid-operation
lost cross-thread completions and deadlocked Skate 3. `xendroid_emu.cpp:840`
calls it straight from the main thread. `ae::quit()` (`xendroid_emu.cpp:861`) is
empty.

## The cause, as far as the code shows it

My reading, not verified on the phone: the owner pressed home (or the screen
went off) rather than quitting. `onStop` paused the core, which stopped audio
and the GPU. A guest thread that sleeps in a kernel wait then stops costing
anything, but a guest thread that busy-waits does not: Fable II's 3D engine
thread spends most of its time in one busy wait (`sub_82B9BF90`, the run 90
profile in `OVERNIGHT-2.md`) and its CPU 3 thread was at 99% in play. With the
GPU and vblank parked, what they wait for never comes, so they spin for ever:
`Guest CPU 0` and `Guest CPU 3` at a full core each, no frames. Nothing ends
the process while the activity is only stopped, and nothing is meant to: the
design keeps the game to come back to. (Android's cached-app freezer would
eventually stop such a process on many phones; it plainly did not on the
owner's, perhaps because it is a debug build or the freezer is off there. I
have not checked which.)

Quitting from the in-game menu already ended the process in `onDestroy`, so
that path should not have shown the bug, unless `onDestroy` came late or not
at all. The xe.log of those two sessions would settle which path it was: a
`! EMULATOR PAUSED !` line with no `ending the session` after it is the home
path.

## Is there a pause that stops the guest?

No safe one. What exists:

- `Emulator::Pause` / `Resume`: the soft pause above, already used. It stops
  audio (`audio_system_->Pause()`, the AAudio or OpenSL ES driver and the XMA
  decoder) and the GPU worker, not the guest.
- `cpu::Processor::Pause` / `SuspendAllThreads` (`cpu/processor.cc:940`,
  `:1081`): the debugger's. It suspends the threads in `thread_debug_infos_`
  with `Thread::Suspend`, asserts it is running, and is the very approach the
  comment in `Emulator::Pause` says deadlocked Skate 3. It is also unproven with
  the guest scheduler's fibres (the host threads `Guest CPU n` run many guest
  threads each).
- The guest scheduler (`kernel/guest_scheduler.h`) has no hold for every CPU;
  `ParkSuspended` is for a guest thread the guest itself suspended. A hold at
  the dispatch loop (stop dispatching while a flag is set; spinners reach a
  safepoint and yield) could work, but it is native work in a file other
  branches own, with deadlock risk under the global lock and no way for me to
  test it, so I did not write it.

So, as the brief allows: the game is ended after a grace period when it is
left in the background.

## What changed

- **New `x360/app/src/main/java/xendroid/compose/core/EmuShutdown.kt`**
  (ours): the constants `BACKGROUND_GRACE_MS = 60_000`, `WATCHDOG_MS = 5_000`,
  `WRITES_WAIT_MS = 2_000`; a counter of app-side writes in flight
  (`tracked { }`, `awaitWrites`); `startWatchdog`, a daemon thread that kills
  the process `WATCHDOG_MS` after the shutdown starts whatever happens;
  `exitAfterWrites`, which waits for the tracked writes (at most
  `WRITES_WAIT_MS`) off the main thread, then `Process.killProcess`.
- **`EmulatorHostActivity.kt`** (upstream XenDroid; every change marked
  `hakuX-Boosted:`):
  - `endSession(reason, fromDestroy)`, idempotent: logs `ending the session:
    <reason>` (tag `EmuHost`), starts the watchdog, removes the pending
    runnables, answers the pending guest prompts (keyboard, message box, disc)
    so none holds a guest thread, pauses the core (audio stops, the GPU is
    quiescent; a no-op if `onStop` already paused it), calls `finish()` (unless
    called from `onDestroy` or already finishing) so the system does not bring
    the activity back in a new process, then `EmuShutdown.exitAfterWrites`.
  - Quit in the in-game menu (touch `onQuit` and the controller's
    `PAUSE_OPTION_QUIT`) calls `quitGame()`, which is `endSession`: the process
    ends at once instead of whenever `onDestroy` comes.
  - `onStop` posts `endInBackground` for `BACKGROUND_GRACE_MS` (not when
    finishing); `onStart` removes it. So home, another app, or the screen off
    for under a minute keep the game, paused, as before; a minute or more ends
    it, with no notification. The next launch boots afresh.
  - `onDestroy` calls `endSession` (it used to kill the process directly, with
    no watchdog). So a destroyed activity (finish, the task removed from
    recents when the system calls `onDestroy`) ends the same way.
  - `ending` guards the four `resumeIfPaused()` calls, so a focus change or a
    surface coming back during the shutdown cannot restart audio.
  - `toggleTouchOverlay`'s config write is `EmuShutdown.tracked`.
- **`x360/app/src/main/java/xendroid/compose/ui/pause/InGameSettings.kt`**
  (upstream XenDroid, marked): the per-title config write in `advance` and the
  controls' `gamepad.save` are `EmuShutdown.tracked`, so a quit straight after
  changing a setting waits for the write.

Not changed: the native core, the JNI bridge, the manifest, the soft pause
itself, the in-game menu's pause, and the disc swap, keyboard and message box
panels (they do not stop the activity, so the grace period never runs while
they are up; only their answers at shutdown are as before).

### Why this is a clean shutdown

The guest's saves go through `HostPathFile` to `pwrite` with no user-space
buffer (`base/filesystem_posix.cc:171`), so whatever the guest has written is
in the kernel's page cache and survives the process ending, by SIGKILL or
otherwise. The core's own config writes (`SaveConfig`, `SaveGameConfig`) and
the app's (`ConfigFiles.writeAtomic`) write a `.tmp` and rename it, so they are
never left half written; what the shutdown waits for is an app write that has
not happened yet. The process still ends with `Process.killProcess`, as it
always did, because `System.exit` runs the C++ static destructors, which have
deadlocked joining a paused audio worker. What is lost: a save the guest is in
the middle of writing at the moment the process ends (the same as before, and
the reason the grace period is a minute and not seconds), and any log lines
still in xe.log's buffer.

## For `boosted/docs/ARCHITECTURE.md` (not edited here)

| Upstream file | Change |
|---|---|
| `x360/app/.../EmulatorHostActivity.kt` | Quit, `onDestroy` and a game stopped for `EmuShutdown.BACKGROUND_GRACE_MS` go through `endSession`: prompts answered, core paused, activity finished, tracked writes waited for, process killed, under a watchdog. The soft pause leaves busy-waiting guest threads spinning, so a stopped game cannot be kept indefinitely |
| `x360/app/.../ui/pause/InGameSettings.kt` | The per-title config write and the controls save are `EmuShutdown.tracked` |

New file (ours): `x360/app/.../core/EmuShutdown.kt`.

## To check on the device

Package `com.rfandango.haku_x.debug`. Before the first run: copy the config
and `content/` (the saves) to the PC as usual.

1. **Quit from the in-game menu.** Start a title, play a little, open the menu,
   Quit. Then:
   - `adb shell pidof com.rfandango.haku_x.debug:emu` prints nothing within a
     second or two (the watchdog's limit is 5 s);
   - `adb logcat -d -s EmuHost EmuShutdown` shows `ending the session: quit
     from the in-game menu` then `session ended (quit from the in-game menu);
     :emu exits`, and no `not over after 5000 ms` (that line means a step hung
     and the watchdog killed it: say which step from the lines before it).
2. **Home, back within the minute.** Start a title, press home. At once:
   - `adb shell pidof com.rfandango.haku_x.debug:emu` still prints a pid;
   - `adb shell top -H -b -n 1 -p $(adb shell pidof com.rfandango.haku_x.debug:emu)`
     (or `top -H -p <pid>` interactively): `Guest CPU 0` and `Guest CPU 3` may
     well still be near a full core here, that is the soft pause's limit and
     why the grace period exists; audio must be silent. Note what they show;
   - go back to the game within 60 s: it is where it was, paused only if the
     menu was open, audio back.
3. **Home, away for longer.** Press home and wait 70 s. Then `pidof` prints
   nothing; logcat shows `ending the session: stopped for 60 s`. Open hakuX
   again: the library, not the game; starting the title boots it afresh.
4. **Screen off** for over a minute in a game: the same as 3 (the game is
   ended). Under a minute: the same as 2.
5. **Swipe the task from recents** while in a game: `pidof` prints nothing.
6. **Saves intact.** In a title that saves (Fable II), save in game, quit from
   the menu, start again and load: the save is there. Then save, press home,
   wait 70 s, start again and load. Compare `content/` with the copy on the PC
   if anything looks wrong.
7. **The panels still work over the game:** the in-game menu (pause, resume,
   settings, edit controls, quit), a disc change where a title asks for one,
   the guest keyboard and a message box. None of them should end the game,
   however long they stay up.
8. In the xe.log of a home session: `! EMULATOR PAUSED !` at home, `!
   EMULATOR RESUMED !` on return.

## Not done, worth doing

- A real hold of the guest (see "Is there a pause that stops the guest?"),
  which would let a stopped game be kept for longer at no cost.
- While the game is stopped and within the grace period, the two spinning
  threads still burn up to a minute of two cores. If that matters, the grace
  period can be shortened (one constant), or their host threads lowered in
  priority while stopped.
- Launching a different title from hakuX's library within the grace period
  brings the old game back (the host activity is `singleTask` and does not
  handle `onNewIntent`). Unchanged by this branch.
