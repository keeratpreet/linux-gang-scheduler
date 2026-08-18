# Integrating into the kernel tree

`gang.c` / `gang_sched.h` are new files; the rest of this is the minimal set
of edits `gang_sched_class` needs from the existing scheduler core to be
recognized as a real, dispatchable class. Developed against a mainline
kernel with the post-EEVDF `pick_task()`/`sched_class_above()` scheduler
core (v6.6+) — line numbers will drift across kernel versions, so these are
given as anchored snippets rather than a line-numbered patch.

## 1. New files

```
kernel/sched/gang.c
kernel/sched/gang_sched.h
```

## 2. `kernel/sched/Makefile`

```diff
 obj-y += core.o
 obj-y += fair.o
+obj-y += gang.o
 obj-y += build_policy.o
 obj-y += build_utility.o
```

## 3. `include/uapi/linux/sched.h` — new policy number

```diff
 #define SCHED_IDLE		5
 #define SCHED_DEADLINE		6
 #define SCHED_EXT		7
+#define SCHED_GANG		8
```

## 4. `include/linux/sched.h` — per-task gang state

```diff
+struct sched_gang_entity {
+	int			gang_id;
+	u64			budget_ns;
+	u64			used_ns;
+	struct list_head	cpu_link;
+};
+
 struct task_struct {
 	...
 	struct sched_entity		se;
+	struct sched_gang_entity	gang;
 	struct sched_rt_entity		rt;
 	struct sched_dl_entity		dl;
```

## 5. `kernel/sched/sched.h` — per-CPU run queue state

```diff
+#include "gang_sched.h"
+
 struct rq {
 	...
 	struct cfs_rq		cfs;
+	struct gang_rq		gang;
 	struct rt_rq		rt;
 	struct dl_rq		dl;
 	...
 };

+extern void gang_cpu_queue_init(struct gang_cpu_queue *q);
 extern void init_cfs_rq(struct cfs_rq *cfs_rq);
 extern void init_rt_rq(struct rt_rq *rt_rq);
```

(the field is declared `struct gang_rq gang;` for consistency with `cfs`/
`rt`/`dl`, aliasing `struct gang_cpu_queue` from `gang_sched.h`.)

## 6. `kernel/sched/core.c` — wire the class into the scheduler

Policy → class mapping, used by `sched_setscheduler()`:

```diff
+extern const struct sched_class gang_sched_class;
+
 const struct sched_class *__setscheduler_class(int policy, int prio)
 {
 	...
+	if (policy == SCHED_GANG)
+		return &gang_sched_class;
+
 	return &fair_sched_class;
 }
```

Per-CPU init, inside the existing `for_each_possible_cpu` loop in
`sched_init()`:

```diff
 		init_cfs_rq(&rq->cfs);
+		gang_cpu_queue_init(&rq->gang);
 		init_rt_rq(&rq->rt);
```

Global registry init, near the end of `sched_init()` alongside the other
`init_sched_*_class()` calls:

```diff
 	init_sched_fair_class();
 	init_sched_ext_class();
+	gang_registry_init();
 	psi_init();
```

## 7. `include/asm-generic/vmlinux.lds.h` — class priority ordering

The scheduler's `for_each_active_class()` walks classes in link order, so
placement in this list *is* the priority order — above `fair`, below `rt`:

```diff
 	*(__stop_sched_class)			\
 	*(__dl_sched_class)			\
 	*(__rt_sched_class)			\
+	*(__gang_sched_class)			\
 	*(__fair_sched_class)			\
 	*(__ext_sched_class)			\
 	*(__idle_sched_class)			\
```

## 8. `arch/x86/entry/syscalls/syscall_64.tbl` — new syscalls

Pick the next free numbers past the table's current end (467-469 assumed
free here; check `tail` of the real table on the target kernel version):

```diff
+
+# gang scheduling
+467	common	register_gang		sys_register_gang
+468	common	exit_gang		sys_exit_gang
+469	common	list			sys_list
```

`register_gang`/`exit_gang`/`list` need matching declarations in
`include/linux/syscalls.h` (`asmlinkage long sys_register_gang(pid_t pid, int gang_id, int budget_secs);` etc.) — omitted here since the exact
macro-generated signature only needs to match the `SYSCALL_DEFINEn` in
`gang.c`.

## Build

```bash
make menuconfig   # enable whatever base config the target machine needs
make -j$(nproc)
make modules_install && make install
reboot   # into the new kernel
```

## Status

**Not compiled or boot-tested** — there's no Linux kernel build toolchain or
bootable VM in this environment.

What *has* been checked: every kernel symbol `gang.c` calls or every
insertion point this document names was cross-referenced line-by-line
against the real kernel source at **tag `v6.12`** (pulled directly from
`github.com/torvalds/linux`), matching the era of the assignment this
project reimplements the spec of. All of the following matched exactly at
that tag:

- `struct sched_class` hook signatures used here (`enqueue_task`,
  `dequeue_task` → `bool`, `yield_task`, `wakeup_preempt`, `pick_task(rq)`
  — single-argument at this tag, `put_prev_task`, `set_next_task`,
  `select_task_rq`, `set_cpus_allowed`, `task_tick`, `switched_to`,
  `switched_from`, `update_curr`).
- `check_class_changing()` / `check_class_changed()` — exist at this tag
  with the signatures used in `gang.c`.
- Free functions: `dequeue_task`, `enqueue_task`, `set_next_task`,
  `task_rq_lock`/`task_rq_unlock`, `find_get_task_by_vpid`,
  `task_on_rq_queued`, `resched_curr`, `add_nr_running`/`sub_nr_running`,
  `update_curr_common`, `rq_clock_task`, `set_cpus_allowed_common`,
  `__setscheduler_class`.
- `DEQUEUE_MOVE`/`DEQUEUE_SAVE`/`ENQUEUE_MOVE`/`ENQUEUE_RESTORE` flags.
- `struct task_struct` field order (`se; rt; dl; dl_server;`) — the `gang`
  insertion point in step 4 is correct for this layout.
- `sched_init()` insertion points in step 6 — `init_cfs_rq(&rq->cfs)` is
  immediately followed by `init_rt_rq`/`init_dl_rq` in that order, and
  `init_sched_fair_class(); init_sched_ext_class();` is immediately
  followed by `psi_init();` — exactly where this doc places
  `gang_cpu_queue_init()` and `gang_registry_init()`.
- `vmlinux.lds.h`'s `SCHED_DATA` macro (step 7), including the
  `__stop_sched_class` entry before `__dl_sched_class`.
- `SCHED_EXT = 7` is the last defined policy in `include/uapi/linux/sched.h`
  at this tag, confirming `SCHED_GANG = 8` is free.
- `arch/x86/entry/syscalls/syscall_64.tbl`'s last real (non-x32) entry at
  this tag is `462  common  mseal`, confirming 463+ is free — so the
  467-469 numbers picked in step 8 are valid at `v6.12`. Re-check the tail
  of the table on whatever kernel you actually target, since it grows with
  every release.

**One thing to know if you're targeting something newer than v6.12:** on
today's bleeding-edge kernel `master` (post-v6.12, unreleased as of this
check), the scheduler core has been refactored again —
`check_class_changing()`/`check_class_changed()` no longer exist, replaced
by a `sched_change_begin()`/`sched_change_end()` pattern with new
`switching_from`/`switching_to` hooks, and `pick_task` gained a second
`struct rq_flags *rf` argument. If the target kernel is newer than v6.12,
`gang_attach()`/`gang_detach()` and `pick_task_gang()` in `gang.c` would
need updating to match — check `kernel/sched/core.c` on the actual target
tag before building.
