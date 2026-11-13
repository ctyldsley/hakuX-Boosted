# Guest waits (branch `claude/guest-waits`)

Nothing here has been run on a device. The only check is the GitHub Actions
build of this branch. No speed-up and no fix is claimed.

Base: `claude/integration-1113` at `8e03766`.

## Done / in progress / next

- Done: step 1, the current-thread and current-CPU lookups (below).
- In progress: step 2 (the yield path and parking), step 3 (busy waits in
  translated code), step 4 (the skew bound at start-up), step 5 (`host perf`
  and the GPU command thread).
- Next: see the end of this file once the steps are in.

## Step 1: the lookups

Behind the existing `host_thread_state_slot` (CPU group, default false):

- `CurrentCpu()` in `kernel/guest_scheduler.cc` reads
  `HostThreadState::scheduler_cpu` instead of the `thread_local`.
- JIT hooks (the safepoint, the spin-wait backoff site) and
  `NtYieldExecution` find their guest thread from the PPC context they are
  handed (`context->thread_state->owner()`, `XThread::FromContext`), with no
  thread-local lookup at all. `host_thread_state_verify` compares it with
  the thread-local answer on every call.
- `NtYieldExecution` passes its context to the kernel APC test, which
  otherwise looked the bound context up through the resolver. This one is
  not behind the setting: it is the same test on the same context.

The rest of this file is written as the steps land.
