/* kernel/sched/gang.c
 *
 * Gang scheduling class: coscheduling for HPC-style workloads.
 *
 * A "gang" is a set of tasks that should be dispatched on their CPUs
 * *simultaneously* so they can spin/busy-wait on each other (barriers,
 * lock-step SPMD phases, MPI ranks sharing a node) without a task being
 * scheduled out mid-barrier while its gang-mates wait for it. This class
 * sits directly above CFS in scheduling priority, so any runnable gang
 * task always preempts a SCHED_NORMAL task, but below RT/deadline classes.
 *
 * Design summary:
 *   - CPU 0 is the "governor". On every dispatch it picks the next gang
 *     in round-robin order from its own queue and, once per GANG_QUANTUM_NS,
 *     broadcasts that gang's id to every other CPU via IPI.
 *   - A worker CPU (any CPU != governor) only ever runs a gang task if it's
 *     still inside the time window opened by the last IPI, and only a task
 *     belonging to the gang the IPI named. This is what makes dispatch
 *     synchronized: every CPU is bound to the same gang for the same
 *     GANG_QUANTUM_NS window.
 *   - A global registry (hashed by gang_id, one rwlock) tracks which pids
 *     belong to which gang, independent of each CPU's local run queue, so
 *     `list_gang_members()` can answer without touching per-CPU state.
 *   - Each task registers with an execution-time budget; task_tick_gang()
 *     demotes it back to CFS once that budget is exhausted.
 */
#include "sched.h"
#include "gang_sched.h"

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

#define GANG_GOVERNOR_CPU	0
#define GANG_QUANTUM_NS		(10ULL * NSEC_PER_MSEC)

#define GANG_HASH_BITS		4
#define GANG_HASH_SIZE		(1U << GANG_HASH_BITS)

#define E_NOSUCHTASK		-23
#define E_TOOMANY		-22

const struct sched_class gang_sched_class;

/* ------------------------------------------------------------------ *
 * Gang registry: gang_id -> list of member pids. Independent of the
 * per-CPU run-queue lists so list()/register/exit don't need every
 * CPU's rq lock, just this one rwlock.
 * ------------------------------------------------------------------ */

struct gang_group {
	int			gang_id;
	unsigned int		member_count;
	struct list_head	hash_link;	/* into gang_table[hash(gang_id)] */
	struct list_head	members;	/* of struct gang_member */
};

struct gang_member {
	pid_t			pid;
	struct list_head	link;
};

static struct list_head gang_table[GANG_HASH_SIZE];
static rwlock_t gang_table_lock;

void gang_registry_init(void)
{
	unsigned int i;

	for (i = 0; i < GANG_HASH_SIZE; i++)
		INIT_LIST_HEAD(&gang_table[i]);
	rwlock_init(&gang_table_lock);
}

static inline struct list_head *gang_bucket(int gang_id)
{
	return &gang_table[((unsigned int)gang_id) & (GANG_HASH_SIZE - 1)];
}

/* caller holds gang_table_lock */
static struct gang_group *gang_group_find(int gang_id)
{
	struct gang_group *g;

	list_for_each_entry(g, gang_bucket(gang_id), hash_link)
		if (g->gang_id == gang_id)
			return g;
	return NULL;
}

static struct gang_group *gang_group_find_or_create(int gang_id)
{
	struct gang_group *g = gang_group_find(gang_id);

	if (g)
		return g;

	g = kzalloc(sizeof(*g), GFP_ATOMIC);
	if (!g)
		return NULL;
	g->gang_id = gang_id;
	INIT_LIST_HEAD(&g->members);
	list_add(&g->hash_link, gang_bucket(gang_id));
	return g;
}

static unsigned int gang_group_size(int gang_id)
{
	struct gang_group *g = gang_group_find(gang_id);

	return g ? g->member_count : 0;
}

static int gang_group_add_member(int gang_id, pid_t pid)
{
	struct gang_group *g = gang_group_find_or_create(gang_id);
	struct gang_member *m;

	if (!g)
		return -ENOMEM;

	m = kzalloc(sizeof(*m), GFP_ATOMIC);
	if (!m)
		return -ENOMEM;

	m->pid = pid;
	list_add_tail(&m->link, &g->members);
	g->member_count++;
	return 0;
}

static void gang_group_remove_member(int gang_id, pid_t pid)
{
	struct gang_group *g = gang_group_find(gang_id);
	struct gang_member *m, *tmp;

	if (!g)
		return;

	list_for_each_entry_safe(m, tmp, &g->members, link) {
		if (m->pid == pid) {
			list_del(&m->link);
			kfree(m);
			g->member_count--;
			break;
		}
	}
	if (g->member_count == 0) {
		list_del(&g->hash_link);
		kfree(g);
	}
}

/* ------------------------------------------------------------------ *
 * Policy switch helpers. Both are called with p's rq lock already held
 * (see the syscalls below), matching how the kernel's own
 * __sched_setscheduler() moves a task between classes.
 * ------------------------------------------------------------------ */

static int gang_attach(struct task_struct *p, struct rq *rq, int gang_id,
			unsigned int budget_secs)
{
	const struct sched_class *prev_class;
	bool queued, running;

	write_lock(&gang_table_lock);
	if (gang_group_size(gang_id) >= (unsigned int)num_possible_cpus()) {
		write_unlock(&gang_table_lock);
		return E_TOOMANY;
	}
	if (gang_group_add_member(gang_id, p->pid)) {
		write_unlock(&gang_table_lock);
		return -ENOMEM;
	}
	write_unlock(&gang_table_lock);

	queued  = task_on_rq_queued(p);
	running = (rq->curr == p);

	if (queued)
		dequeue_task(rq, p, DEQUEUE_MOVE | DEQUEUE_SAVE);
	if (running)
		p->sched_class->put_prev_task(rq, p, NULL);

	prev_class = p->sched_class;
	p->sched_class    = &gang_sched_class;
	p->policy         = SCHED_GANG;
	p->gang.gang_id   = gang_id;
	p->gang.budget_ns = (u64)budget_secs * NSEC_PER_SEC;
	p->gang.used_ns   = 0;

	check_class_changing(rq, p, prev_class);
	if (queued)
		enqueue_task(rq, p, ENQUEUE_MOVE | ENQUEUE_RESTORE);
	if (running) {
		/* Force a fresh IPI round the next time this CPU dispatches,
		 * rather than waiting out whatever quantum was already
		 * in flight for a different gang. */
		if (rq->cpu == GANG_GOVERNOR_CPU)
			rq->gang.last_ipi_ns = 0;
		set_next_task(rq, p);
	}
	check_class_changed(rq, p, prev_class, p->prio);

	return 0;
}

static void gang_detach(struct task_struct *p, struct rq *rq)
{
	const struct sched_class *prev_class = p->sched_class;
	bool queued  = task_on_rq_queued(p);
	bool running = (rq->curr == p);

	if (queued)
		dequeue_task(rq, p, DEQUEUE_MOVE | DEQUEUE_SAVE);
	if (running)
		p->sched_class->put_prev_task(rq, p, NULL);

	p->sched_class = &fair_sched_class;
	p->policy      = SCHED_NORMAL;

	check_class_changing(rq, p, prev_class);
	if (queued)
		enqueue_task(rq, p, ENQUEUE_MOVE | ENQUEUE_RESTORE);
	if (running)
		set_next_task(rq, p);
	check_class_changed(rq, p, prev_class, p->prio);

	write_lock(&gang_table_lock);
	gang_group_remove_member(p->gang.gang_id, p->pid);
	write_unlock(&gang_table_lock);
}

/* ------------------------------------------------------------------ *
 * Syscalls
 * ------------------------------------------------------------------ */

SYSCALL_DEFINE3(register_gang, pid_t, pid, int, gang_id, int, budget_secs)
{
	struct task_struct *p;
	struct rq_flags rf;
	struct rq *rq;
	int ret;

	p = find_get_task_by_vpid(pid);
	if (!p)
		return E_NOSUCHTASK;

	rq = task_rq_lock(p, &rf);
	ret = gang_attach(p, rq, gang_id, budget_secs);
	task_rq_unlock(rq, p, &rf);
	put_task_struct(p);

	return ret;
}

SYSCALL_DEFINE1(exit_gang, pid_t, pid)
{
	struct task_struct *p;
	struct rq_flags rf;
	struct rq *rq;

	p = find_get_task_by_vpid(pid);
	if (!p)
		return E_NOSUCHTASK;

	rq = task_rq_lock(p, &rf);
	gang_detach(p, rq);
	task_rq_unlock(rq, p, &rf);
	put_task_struct(p);

	return 0;
}

SYSCALL_DEFINE2(list, int, gang_id, pid_t __user *, out)
{
	struct gang_group *g;
	struct gang_member *m;
	unsigned int n = 0;

	read_lock(&gang_table_lock);
	g = gang_group_find(gang_id);
	if (g) {
		list_for_each_entry(m, &g->members, link) {
			if (put_user(m->pid, out + n))
				break;
			n++;
		}
	}
	read_unlock(&gang_table_lock);

	return n;
}

/* ------------------------------------------------------------------ *
 * sched_class hooks
 * ------------------------------------------------------------------ */

void gang_cpu_queue_init(struct gang_cpu_queue *q)
{
	INIT_LIST_HEAD(&q->members);
}

static inline struct sched_gang_entity *gang_se_of(struct list_head *l)
{
	return list_entry(l, struct sched_gang_entity, cpu_link);
}

static inline struct task_struct *gang_task_of(struct sched_gang_entity *ge)
{
	return container_of(ge, struct task_struct, gang);
}

static void enqueue_task_gang(struct rq *rq, struct task_struct *p, int flags)
{
	list_add_tail(&p->gang.cpu_link, &rq->gang.members);
	rq->gang.nr_members++;
	add_nr_running(rq, 1);
}

static bool dequeue_task_gang(struct rq *rq, struct task_struct *p, int flags)
{
	list_del(&p->gang.cpu_link);
	rq->gang.nr_members--;
	sub_nr_running(rq, 1);
	return true;
}

static void yield_task_gang(struct rq *rq)
{
	/* Nothing to do: the governor's round robin already rotates gangs
	 * on its own quantum; an individual task can't jump the queue. */
}

static void wakeup_preempt_gang(struct rq *rq, struct task_struct *p, int flags)
{
	u64 now = sched_clock();
	bool in_window = (rq->cpu == GANG_GOVERNOR_CPU) ||
			  (now - rq->gang.last_ipi_ns < GANG_QUANTUM_NS);

	if (rq->gang.nr_members && in_window &&
	    sched_class_above(&gang_sched_class, rq->curr->sched_class))
		resched_curr(rq);
}

/* governor only: advance to the next gang in round-robin order */
static void gang_rotate(struct rq *rq)
{
	if (!list_empty(&rq->gang.members))
		list_move_tail(rq->gang.members.next, &rq->gang.members);
}

static struct task_struct *pick_task_gang(struct rq *rq)
{
	u64 now = sched_clock();

	if (rq->cpu == GANG_GOVERNOR_CPU) {
		if (list_empty(&rq->gang.members))
			return NULL;
		if (now - rq->gang.last_rotate_ns > GANG_QUANTUM_NS) {
			gang_rotate(rq);
			rq->gang.last_rotate_ns = now;
		}
		return gang_task_of(gang_se_of(rq->gang.members.next));
	}

	/* Worker CPU: only offer a task if we're still inside the window
	 * opened by the governor's last IPI, and only from the gang it
	 * named — this is what keeps every CPU's dispatch synchronized. */
	if (now - rq->gang.last_ipi_ns < GANG_QUANTUM_NS) {
		struct sched_gang_entity *ge;

		list_for_each_entry(ge, &rq->gang.members, cpu_link) {
			if (ge->gang_id == rq->gang.active_gang)
				return gang_task_of(ge);
		}
	}
	return NULL;
}

static void put_prev_task_gang(struct rq *rq, struct task_struct *p,
				struct task_struct *next)
{
	update_curr_common(rq);
}

/* Runs on a worker CPU in IPI context when the governor dispatches. */
static void gang_ipi_handler(void *info)
{
	int gang_id = (int)(long)info;
	struct rq *rq = this_rq();

	rq->gang.last_ipi_ns = sched_clock();
	rq->gang.active_gang = gang_id;

	if (rq->gang.nr_members &&
	    (sched_class_above(&gang_sched_class, current->sched_class) ||
	     (current->sched_class == &gang_sched_class &&
	      current->gang.gang_id != gang_id)))
		resched_curr(rq);
}

static void set_next_task_gang(struct rq *rq, struct task_struct *p, bool first)
{
	u64 now = sched_clock();

	p->se.exec_start = rq_clock_task(rq);

	if (rq->cpu != GANG_GOVERNOR_CPU)
		return;

	/* Only the governor sends IPIs, and only once per quantum — this
	 * is what turns "a task became runnable" into "every CPU agrees
	 * on which gang gets this quantum," without re-broadcasting on
	 * every single reschedule. */
	if (now - rq->gang.last_ipi_ns > GANG_QUANTUM_NS) {
		rq->gang.last_ipi_ns = now;
		smp_call_function(gang_ipi_handler,
				   (void *)(long)p->gang.gang_id, 0);
	}
}

static int select_task_rq_gang(struct task_struct *p, int cpu, int flags)
{
	/* Placement is decided once, at register_gang() time (the caller's
	 * current CPU); the gang class doesn't migrate tasks on wakeup. */
	return cpu;
}

static void update_curr_gang(struct rq *rq)
{
	if (rq->curr->sched_class == &gang_sched_class)
		rq->curr->gang.used_ns += update_curr_common(rq);
}

static void task_tick_gang(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_gang(rq);

	if (p->gang.used_ns > p->gang.budget_ns) {
		pr_info("gang: pid %d (gang %d) exceeded its %llu ns budget "
			"(used %llu ns) -- demoting to CFS\n",
			p->pid, p->gang.gang_id, p->gang.budget_ns,
			p->gang.used_ns);
		gang_detach(p, rq);
		resched_curr(rq);
		return;
	}

	if (rq->cpu == GANG_GOVERNOR_CPU &&
	    sched_clock() - rq->gang.last_rotate_ns > GANG_QUANTUM_NS)
		resched_curr(rq);
}

static void switched_to_gang(struct rq *rq, struct task_struct *p)
{
}

static void switched_from_gang(struct rq *rq, struct task_struct *p)
{
}

DEFINE_SCHED_CLASS(gang) = {
	.enqueue_task		= enqueue_task_gang,
	.dequeue_task		= dequeue_task_gang,
	.yield_task		= yield_task_gang,

	.wakeup_preempt		= wakeup_preempt_gang,

	.pick_task		= pick_task_gang,
	.put_prev_task		= put_prev_task_gang,
	.set_next_task		= set_next_task_gang,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_gang,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif

	.task_tick		= task_tick_gang,

	.switched_to		= switched_to_gang,
	.switched_from		= switched_from_gang,
	.update_curr		= update_curr_gang,
};
