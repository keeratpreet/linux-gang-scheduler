# Linux Kernel Gang Scheduler

A custom Linux scheduling class (`gang_sched_class`) that adds coordinated
gang scheduling on top of the mainline scheduler — for HPC-style workloads
where a set of cooperating threads (MPI ranks on one node, SPMD worker
threads sharing a barrier) need to run *simultaneously* across cores rather
than being scheduled independently by CFS.

## The problem this solves

CFS schedules each thread independently based on fairness/vruntime. That's
the right default for general-purpose workloads, but it's a poor fit for
tightly-coupled parallel jobs: if thread A reaches a barrier and thread B
(on another core) gets preempted by an unrelated task before it reaches the
same barrier, A just burns CPU spin-waiting — the whole gang's progress is
gated by whichever member the scheduler happens to be running. **Gang
scheduling** fixes this by dispatching every member of a gang together, for
a fixed quantum, across all cores at once.

## Architecture

**A new `sched_class`, above CFS.** `gang_sched_class` is linked into the
scheduler's class-priority list (`vmlinux.lds.h`) directly above
`fair_sched_class` and below `rt`/`deadline` — so a runnable gang task
always preempts SCHED_NORMAL work, but real-time tasks still win. This is
the standard extension point the kernel provides (`sched_ext` uses the same
mechanism); implementing a new class means providing the ops table in
`DEFINE_SCHED_CLASS(gang) = { .enqueue_task = ..., .pick_task = ..., ... }`
and nothing else touches scheduling structurally.

**CPU 0 is the governor.** Every other CPU is a worker. On every dispatch,
CPU 0 round-robins to the next gang queued on it and — at most once per
10ms quantum — broadcasts that gang's id to all worker CPUs via
`smp_call_function()` (an IPI). This is the actual coscheduling mechanism:
a worker CPU's `pick_task_gang()` will **only** return a task if (a) it's
still inside the 10ms window opened by the last IPI it received, and (b)
that task belongs to the gang the IPI named. Every CPU is therefore locked
to the same gang for the same window, achieving synchronized dispatch
without a shared run queue or a global lock on the scheduling hot path.

**Registry, separate from the run queues.** `register_gang`/`exit_gang`
need to answer "how many members does gang N already have" and "who's in
gang N" (for `list()`) without walking every CPU's local run queue. A
global hash table (`gang_table`, bucketed by `gang_id`, one `rwlock_t`)
tracks membership independently of where each member is actually queued.
Reads (`list()`) take the read lock; registering/exiting take the write
lock — contention is expected to be low since gang membership changes are
rare relative to scheduling decisions.

**Per-task execution budget.** `register_gang(pid, gang_id, budget_secs)`
sets a time budget in seconds; `task_tick_gang()` accumulates actual
runtime and demotes the task back to `SCHED_NORMAL`/CFS once it's
exceeded, logging why. This bounds how long a misbehaving or long-running
gang member can hold elevated scheduling priority.

## Syscalls

| Syscall | Signature | Behavior |
|---|---|---|
| `register_gang` | `(pid_t pid, int gang_id, int budget_secs)` | Moves `pid` onto `gang_sched_class`, joining/creating `gang_id` with a `budget_secs` execution budget. Returns `-22` if the gang already has `num_possible_cpus()` members (a gang can't be larger than the machine can coschedule), `-23` if `pid` doesn't exist. |
| `exit_gang` | `(pid_t pid)` | Moves `pid` back to CFS (`SCHED_NORMAL`) and removes it from the registry. |
| `list` | `(int gang_id, pid_t *out)` | Writes every pid in `gang_id` into the caller's `out` buffer, returns the count. |

See `demo_gang.c` for a minimal userspace client using these directly via
`syscall(2)`.

## Files

```
gang.c            kernel/sched/gang.c        -- the scheduling class + syscalls (new file)
gang_sched.h      kernel/sched/gang_sched.h  -- per-CPU queue struct (new file)
INTEGRATION.md    exact edits needed in core.c, sched.h, the syscall table,
                  Makefile, and vmlinux.lds.h to wire the class into a
                  real kernel tree
demo_gang.c       userspace demo client for the three syscalls
```

`gang.c`/`gang_sched.h` are the only new kernel files this project adds —
everything else is a small, precisely-scoped set of edits to existing
kernel files, documented in `INTEGRATION.md` rather than vendored here in
full (the base files are the actual Linux kernel source, not something to
duplicate wholesale in this repo).

## Build

This is kernel code — it doesn't build standalone. To use it:

1. Get a kernel source tree (`git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git`, or your distro's source package), kernel ≥ 6.6 (needs the `pick_task()`/`sched_class_above()`-based scheduler core).
2. Copy `gang.c` and `gang_sched.h` into `kernel/sched/`.
3. Apply the edits described in `INTEGRATION.md`.
4. `make menuconfig && make -j$(nproc) && make modules_install && make install`, then boot into it.

`demo_gang.c` builds as an ordinary userspace program once the new kernel
is running: `gcc -O2 -o demo_gang demo_gang.c`.

**Not build/boot-tested in this environment** — there's no Linux kernel
build toolchain or bootable VM available here (this was written on macOS).
Review the hook implementations against your actual kernel version's
`struct sched_class`/`struct rq` layout before building.

## Known limitations

- Round-robin between gangs is fully all-or-nothing: two gangs can't run
  concurrently on disjoint cores even if their core requirements don't
  overlap. A ticket/lease-based admission scheme could let non-overlapping
  gangs interleave instead of strictly time-sharing the whole machine.
- The governor (CPU 0) can't safely sleep on a lock held by one of its own
  gang's threads on another core: since only the governor sends IPIs, a
  blocked governor means workers never get told to switch away from a
  stale gang. Solving this needs the governor to detect "I'm about to
  block" and hand off governance, which isn't implemented here.
- `register_gang` rejects a gang once it has `num_possible_cpus()` members,
  which assumes one thread per core is the intended usage — oversubscribed
  gangs aren't supported.
