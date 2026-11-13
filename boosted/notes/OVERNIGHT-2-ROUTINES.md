# Overnight 2026-11-13 to 14: the scheduled routines' instructions

Read `CLAUDE.md` and `boosted/notes/OVERNIGHT-2.md` first; their rules bind
you too (UK English, no em dashes, commit style `area: what changed and
why`, no game data in the repository, never claim a result that was not
measured on a device). You run unattended: nobody can answer a question.
When in doubt, do less and write down why.

The branches in play, all cut from `claude/integration-1113`:

| Branch | Notes file |
|---|---|
| `claude/shader-first-run` | `boosted/notes/shader-first-run.md` |
| `claude/guest-waits` | `boosted/notes/guest-waits.md` |
| `claude/translator-wide` | `boosted/notes/translator-wide.md` |
| `claude/defaults-config` | `boosted/notes/defaults-config.md` |
| `claude/bench-harness` (starts 02:15) | `boosted/notes/bench-harness.md` |
| `claude/memory-watches` (starts 02:15) | `boosted/notes/memory-watches.md` |

Check a branch's latest build with the public API, no token needed:
`curl -s "https://api.github.com/repos/ctyldsley/hakuX-Boosted/actions/runs?branch=<branch>&per_page=3"`
(fields `status`, `conclusion`, `head_sha`, `name`). The app build is the
workflow in `.github/workflows/boosted.yml`; other workflows (tests, the
bench) are judged separately.

## Section A: the resume check (02:30 and 04:30)

Purpose: a session that stopped early (a usage limit, an error) must not
leave its work unfinished until morning.

For each of the six branches:

1. `git fetch origin` and look at the branch. If it does not exist yet, or
   its newest commit is less than 90 minutes old, its session is alive or
   has only just stopped: leave it alone and move on.
2. Read its notes file. If the "done / in progress / next" list shows
   nothing in progress and nothing left of what its task in OVERNIGHT-2
   asked for, it is finished: leave it alone.
3. Otherwise it stalled. Pick the ONE stalled branch whose remaining work is
   worth most (shaders first, then guest waits, then the translator, then
   the rest) and carry its work on: check the branch out, read its notes and
   its row in OVERNIGHT-2, obey the same ownership (edit only that row's
   files), continue from "in progress" then "next", commit and push to that
   same branch after each working step, keep its notes current, and wait for
   the app build to be green before you stop. Before every push run
   `git fetch` and, if the branch moved, stop: its own session is back, and
   two writers must not share a branch.
4. If a branch's latest app build failed, fixing that build comes before any
   new work on it.
5. If nothing stalled, say so and stop. Do not start new work of your own.

Write what you did at the end of that branch's notes file under a heading
"Resume check, <time>".

## Section B: the merge and build (07:00)

Purpose: one build with everything that is ready, waiting for the owner in
the morning. `claude/integration-1113` is the known-good branch that is on
the owner's phone: do not push to it.

1. `git fetch origin`. Create `claude/integration-1114` from
   `origin/claude/integration-1113`.
2. For each of the six branches, in this order: `claude/defaults-config`,
   `claude/guest-waits`, `claude/translator-wide`, `claude/shader-first-run`,
   `claude/memory-watches`, `claude/bench-harness`:
   - Skip it if it does not exist, has no commits beyond
     `claude/integration-1113`, or its latest app build is not `success`
     (if a build is still running, wait for it, up to 45 minutes in all).
     A branch whose newest commit is under 20 minutes old may still be being
     written: wait until it has been quiet for 20 minutes, up to the same
     45 minutes, then take it as it is.
   - `git merge --no-ff` it with the message
     `merge: <branch> (<one line on what it adds; its settings default to off>)`.
   - If it conflicts: resolve only where the two sides plainly add separate
     things (two settings added beside each other, two entries in a list).
     Anything else, `git merge --abort`, skip the branch and record why.
     Never resolve a conflict by guessing at behaviour.
3. Do not change any setting's default or any code yourself. The merge adds
   what the branches hold and nothing else.
4. Add a section at the top of "Right now" in `boosted/HANDOFF.md`, dated,
   that lists: each branch merged with its head commit and its own build's
   run number; each branch skipped and why; every new setting by name with
   the branch it came from, taken from the notes files; anything a notes
   file says must be checked on the device first; and the plain statement
   that nothing in this build has run on a device. Also copy each merged
   branch's list of upstream files changed into
   `boosted/docs/ARCHITECTURE.md` where that file lists such changes.
   Commit as `handoff: the overnight branches merged into claude/integration-1114`.
5. Push `claude/integration-1114` and wait for its app build. If it fails,
   find which merge broke it (build errors name files; a branch's own build
   was green, so the fault is an interaction), drop that branch's merge by
   rebuilding the integration branch without it, push, and wait again.
   Repeat until the build is green or only `claude/integration-1113` is
   left. Record what was dropped and the error.
6. End by writing, as the last lines of your HANDOFF section: the run number
   and URL of the green build, and the order in which the owner should
   switch the new settings on, safest first.
