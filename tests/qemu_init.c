// SPDX-License-Identifier: GPL-2.0
/* PID 1 for the QEMU test boot (see tools/run_qemu.sh).
 *
 * Mounts the pseudo filesystems, loads strela2.ko with simulated devices, runs
 * test_strela2, reports the exit status, then unloads the module and reboots
 * (QEMU is started with -no-reboot, so that exits the VM).
 *
 * Deliberately tiny and static: no busybox, no shell, no rootfs image.
 */

#include <fcntl.h>
#include <stdio.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define MODULE_PATH "/strela2.ko"
#define MODULE_NAME "strela2"
#define TEST_PATH   "/test_strela2"

/* Printed by run_qemu.sh to decide whether the run passed. */
#define RESULT_PREFIX "qemu_init: tests exited "

/* Module parameters are fixed at load time, so each group of tests gets the
 * module loaded the way it needs it. Reloading between rounds also exercises
 * the unload path, including the drain of whatever the round left queued. */
static const struct {
	const char *suite;
	const char *args;
} rounds[] = {
	{
		/* Two devices, jobs fast enough to keep the suite quick. */
		"core",
		"sim_dev_count=2",
	},
	{
		/* Slow jobs, so a submit returns while its job is still queued,
		 * a dependent job is visibly held back, and work is still in
		 * flight at unload. Two devices, so an in-fence can cross
		 * between independent queues. */
		"queue",
		"sim_dev_count=2 sim_job_delay_us=100000",
	},
	{
		/* Longer than STRELA_JOB_TIMEOUT_MS, so jobs time out. */
		"timeout",
		"sim_dev_count=1 sim_job_delay_us=900000",
	},
};

static int load_module(const char *args)
{
	int fd = open(MODULE_PATH, O_RDONLY | O_CLOEXEC);
	int ret;

	if (fd < 0) {
		perror("open " MODULE_PATH);
		return -1;
	}

	ret = syscall(SYS_finit_module, fd, args, 0);
	if (ret)
		perror("finit_module");
	close(fd);
	return ret;
}

static int run_tests(const char *suite)
{
	int status = 0;
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execl(TEST_PATH, TEST_PATH, suite, (char *)NULL);
		perror("execl " TEST_PATH);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void)
{
	int worst = 0;
	size_t i;

	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

	/* The kernel is booted with "quiet" to keep its boot off the console;
	 * from here on everything is wanted, KUnit's results included. */
	klogctl(8 /* SYSLOG_ACTION_CONSOLE_LEVEL */, NULL, 7);

	for (i = 0; i < sizeof rounds / sizeof *rounds; i++) {
		int ret;

		printf("qemu_init: loading " MODULE_NAME " %s\n", rounds[i].args);
		fflush(stdout);

		if (load_module(rounds[i].args)) {
			worst = 255;
			break;
		}

		ret = run_tests(rounds[i].suite);
		printf("qemu_init: suite %s exited %d\n", rounds[i].suite, ret);
		if (ret)
			worst = ret;

		/* Unloading is a test of its own: the drain must fail whatever
		 * the round left queued, without warning or hanging. */
		if (syscall(SYS_delete_module, MODULE_NAME, O_NONBLOCK)) {
			perror("delete_module");
			worst = 255;
			break;
		}
		puts("qemu_init: module unloaded");
		fflush(stdout);
	}

	printf(RESULT_PREFIX "%d\n", worst);
	fflush(stdout);
	sync();
	reboot(RB_AUTOBOOT);
	return 0;
}
