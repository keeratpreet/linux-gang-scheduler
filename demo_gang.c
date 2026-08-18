/* demo_gang.c — userspace demo of the register_gang / list / exit_gang
 * syscalls. Not part of the kernel patch; this runs as a normal process
 * on a kernel that has gang_sched_class built in.
 *
 * Usage: ./demo_gang <gang_id> <budget_secs> <spin_secs>
 *
 * Registers the calling process into <gang_id> with a <budget_secs>
 * execution-time budget, busy-spins for <spin_secs> wall-clock seconds
 * (so you can watch it get co-scheduled with other members of the same
 * gang via `ps -o pid,psr,pri,policy` or /proc/<pid>/sched), then exits
 * the gang and returns to CFS before the process ends.
 *
 * The three syscalls are wired up with raw syscall(2) since they aren't
 * in glibc — swap in the real numbers assigned in syscall_64.tbl
 * (see INTEGRATION.md) if they differ on your build.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_register_gang
#define SYS_register_gang 467
#endif
#ifndef SYS_exit_gang
#define SYS_exit_gang 468
#endif
#ifndef SYS_gang_list
#define SYS_gang_list 469
#endif

static long register_gang(pid_t pid, int gang_id, int budget_secs)
{
	return syscall(SYS_register_gang, pid, gang_id, budget_secs);
}

static long exit_gang(pid_t pid)
{
	return syscall(SYS_exit_gang, pid);
}

/* `cap` isn't passed to the kernel -- the syscall has no length argument,
 * so the caller is responsible for sizing `out` generously enough (the
 * kernel writes one pid per member with no bounds check against `cap`). */
static long gang_list(int gang_id, pid_t *out, size_t cap)
{
	(void)cap;
	return syscall(SYS_gang_list, gang_id, out);
}

static double now_secs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int gang_id, budget_secs, spin_secs;
	pid_t members[64];
	long n, ret;
	double start;

	if (argc < 4) {
		fprintf(stderr, "usage: %s <gang_id> <budget_secs> <spin_secs>\n", argv[0]);
		return 1;
	}
	gang_id     = atoi(argv[1]);
	budget_secs = atoi(argv[2]);
	spin_secs   = atoi(argv[3]);

	ret = register_gang(getpid(), gang_id, budget_secs);
	if (ret != 0) {
		fprintf(stderr, "register_gang failed: %ld (errno=%d)\n", ret, errno);
		return 1;
	}
	printf("pid %d joined gang %d, budget=%ds\n", getpid(), gang_id, budget_secs);

	n = gang_list(gang_id, members, 64);
	printf("gang %d currently has %ld member(s): ", gang_id, n);
	for (long i = 0; i < n; i++)
		printf("%d ", members[i]);
	printf("\n");

	start = now_secs();
	while (now_secs() - start < spin_secs)
		; /* busy-spin so this process stays runnable/CPU-bound */

	exit_gang(getpid());
	printf("pid %d left gang %d after %.1fs\n", getpid(), gang_id, now_secs() - start);

	return 0;
}
