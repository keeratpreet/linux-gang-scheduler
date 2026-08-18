/* kernel/sched/gang_sched.h
 *
 * Per-CPU state for the gang scheduling class. One instance of
 * `struct gang_cpu_queue` is embedded in every `struct rq` (see
 * INTEGRATION.md), the same way `struct cfs_rq` / `struct rt_rq` are.
 */
#ifndef _KERNEL_SCHED_GANG_SCHED_H
#define _KERNEL_SCHED_GANG_SCHED_H

#include <linux/types.h>
#include <linux/list.h>

struct gang_cpu_queue {
	struct list_head	members;	/* sched_gang_entity->cpu_link for every runnable
						 * gang task queued on this CPU */
	u64			last_ipi_ns;	/* sched_clock() when this CPU last received (worker)
						 * or sent (governor) a dispatch IPI */
	u64			last_rotate_ns;	/* governor only: sched_clock() at the last
						 * round-robin advance between gangs */
	int			active_gang;	/* worker only: gang_id named by the governor's
						 * most recent IPI */
	unsigned int		nr_members;	/* fast-path count so wakeup_preempt/task_tick
						 * don't have to walk `members` just to check
						 * "is there any gang work queued here?" */
};

void gang_cpu_queue_init(struct gang_cpu_queue *q);
void gang_registry_init(void);

#endif /* _KERNEL_SCHED_GANG_SCHED_H */
