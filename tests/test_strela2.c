#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <linux/sync_file.h>

#include "kselftest.h"
#include "strela_drm.h"

/* Failures within the test that is currently running. */
static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			ksft_print_msg("%s:%d: ", __func__, __LINE__);         \
			ksft_print_msg(__VA_ARGS__);                           \
			ksft_print_msg("\n");                                  \
			failures++;                                            \
		}                                                              \
	} while (0)

/* ------------------------------------------------------------------------- */

/* Open the n-th STRELA. Accel minors are handed out in probe order and shared
 * with every other accelerator in the system, so the node number means nothing:
 * ask each one what driver is behind it. */
static int strela_open(int nth)
{
	char path[32];
	int found = 0;

	for (int minor = 0; minor < 8; minor++) {
		char name[16] = { 0 };
		struct drm_version version = {
			.name = name,
			.name_len = sizeof name - 1,
		};
		int fd;

		snprintf(path, sizeof path, "/dev/accel/accel%d", minor);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		if (ioctl(fd, DRM_IOCTL_VERSION, &version) == 0 &&
		    strcmp(name, "strela") == 0 && found++ == nth)
			return fd;

		close(fd);
	}

	return -1;
}

#define PARAM_DIR "/sys/module/strela2/parameters/"

/* Module parameters are fixed at load time; tests only read them and adapt.
 * Returns false when the parameter is not there, which is how a test detects
 * real hardware rather than the simulator. */
static bool get_param(const char *name, unsigned int *value)
{
	char path[128];
	char buf[32];
	int fd, len;

	snprintf(path, sizeof path, PARAM_DIR "%s", name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;

	len = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (len <= 0)
		return false;

	buf[len] = '\0';
	return sscanf(buf, "%u", value) == 1;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static int gem_new(int fd, __u64 size, __u32 *handle)
{
	struct drm_strela_gem_new args = { .size = size };
	int ret = ioctl(fd, DRM_IOCTL_STRELA_GEM_NEW, &args);

	if (ret == 0)
		*handle = args.handle;
	return ret;
}

/* Like gem_new, but also maps the buffer: GEM_NEW hands back the fake mmap
 * offset to pass to mmap(2). */
static int gem_new_mapped(int fd, __u64 size, __u32 *handle, void **ptr)
{
	struct drm_strela_gem_new args = { .size = size };

	if (ioctl(fd, DRM_IOCTL_STRELA_GEM_NEW, &args) < 0)
		return -1;

	*ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    args.offset);
	if (*ptr == MAP_FAILED)
		return -1;

	*handle = args.handle;
	return 0;
}

static int gem_sync(int fd, __u32 handle, __u32 flags, __u64 offset, __u64 length)
{
	struct drm_strela_gem_sync args = {
		.handle = handle, .flags = flags,
		.offset = offset, .length = length,
	};

	return ioctl(fd, DRM_IOCTL_STRELA_GEM_SYNC, &args);
}

static int gem_close(int fd, __u32 handle)
{
	struct drm_gem_close args = { .handle = handle };

	return ioctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

/* Sync objects: created by userspace, handed to submit, waited on afterwards. */
static __u32 syncobj_new(int fd)
{
	struct drm_syncobj_create args = { 0 };

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args) < 0)
		return 0;
	return args.handle;
}

static void syncobj_put(int fd, __u32 handle)
{
	struct drm_syncobj_destroy args = { .handle = handle };

	if (handle)
		ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

/* DRM_IOCTL_SYNCOBJ_WAIT takes an absolute CLOCK_MONOTONIC deadline, so a
 * timeout of 0 means "is it signalled right now". */
static bool syncobj_wait(int fd, __u32 handle, int timeout_ms)
{
	struct timespec ts;
	struct drm_syncobj_wait args = {
		.handles = (__u64)(uintptr_t)&handle,
		.count_handles = 1,
		.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
	};

	clock_gettime(CLOCK_MONOTONIC, &ts);
	args.timeout_nsec = (__s64)ts.tv_sec * 1000000000 + ts.tv_nsec +
			    (__s64)timeout_ms * 1000000;

	return ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args) == 0;
}

/* Sync object handles are per open file. Moving a fence to another device means
 * exporting the object as an fd and importing it there. Returns a handle valid
 * on dst_fd, or 0. */
static __u32 syncobj_import(int dst_fd, int src_fd, __u32 handle)
{
	struct drm_syncobj_handle export = { .handle = handle };
	struct drm_syncobj_handle import = { 0 };
	int ret;

	if (ioctl(src_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export) < 0)
		return 0;

	import.fd = export.fd;
	ret = ioctl(dst_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import);
	close(export.fd);

	return ret == 0 ? import.handle : 0;
}

/* A wait says only "signalled", never why. The fence's error is visible through
 * a sync_file export, which is how a test tells a failed job from a good one. */
static int syncobj_status(int fd, __u32 handle)
{
	struct drm_syncobj_handle export = {
		.handle = handle,
		.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
	};
	struct sync_fence_info fence_info = { 0 };
	struct sync_file_info info = {
		.num_fences = 1,
		.sync_fence_info = (__u64)(uintptr_t)&fence_info,
	};
	int ret;

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export) < 0)
		return -errno;

	ret = ioctl(export.fd, SYNC_IOC_FILE_INFO, &info);
	close(export.fd);
	if (ret < 0)
		return -errno;

	return fence_info.status;
}

/* Submit a job reading in_handle and writing out_handle, optionally waiting for
 * in_syncobj first (0 means no dependency). The completion fence lands in
 * out_syncobj. Returns 0, or -1 with errno set. */
static int submit_after(int fd, __u32 config, __u32 in_handle, __u32 out_handle,
			__u32 out_syncobj, __u32 in_syncobj)
{
	struct drm_strela_submit args = {
		.config = { .handle = config, .count = 80, .stride = 0 },
		.out_syncobj = out_syncobj,
		.in_syncobj = in_syncobj,
	};

	if (in_syncobj)
		args.flags |= DRM_STRELA_SUBMIT_WAIT_SYNCOBJ;

	args.inputs[0] = (struct drm_strela_binding){
		.handle = in_handle, .count = 16, .stride = 1,
	};
	args.outputs[0] = (struct drm_strela_binding){
		.handle = out_handle, .count = 16, .stride = 0,
	};

	return ioctl(fd, DRM_IOCTL_STRELA_SUBMIT, &args);
}

/* Submit with a fresh sync object; returns its handle, or 0 with errno set. */
static __u32 submit(int fd, __u32 config, __u32 in_handle, __u32 out_handle)
{
	__u32 sync = syncobj_new(fd);

	if (!sync)
		return 0;

	if (submit_after(fd, config, in_handle, out_handle, sync, 0) < 0) {
		int err = errno;

		syncobj_put(fd, sync);
		errno = err;
		return 0;
	}

	return sync;
}

/* Buffers big enough for the bindings above. */
static int make_buffers(int fd, __u32 *config, __u32 *in, __u32 *out)
{
	if (gem_new(fd, 80 * STRELA_WORD_SIZE, config) ||
	    gem_new(fd, 16 * STRELA_WORD_SIZE, in) ||
	    gem_new(fd, 16 * STRELA_WORD_SIZE, out))
		return -1;
	return 0;
}

/* ------------------------------------------------------------------------- */

/* A job runs to completion and its fence signals. */
static void test_single_job(int fd)
{
	__u32 config, in, out, sync;

	CHECK(make_buffers(fd, &config, &in, &out) == 0, "gem_new failed: %s",
	      strerror(errno));

	sync = submit(fd, config, in, out);
	CHECK(sync != 0, "submit failed: %s", strerror(errno));
	if (!sync)
		return;

	CHECK(syncobj_wait(fd, sync, 2000), "fence never signalled");
	syncobj_put(fd, sync);
}

enum {
	STRELA_PE_ROWS = 4,
	STRELA_PE_COLS = 4,
	STRELA_NPE = STRELA_PE_ROWS * STRELA_PE_COLS,
	STRELA_KERNEL_SIZE = STRELA_NPE * 5,
};
/* Configured for four colums
 */
static const uint32_t bypass_kernel_bitstream[STRELA_KERNEL_SIZE] = {
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 12
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 8
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 4
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 0

	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 13
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 9
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 5
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 1

	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 14
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 10
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 6
	0x00000021, 0x00000000, 0x00000012, 0x00000000, 0x00000000, // 2

	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 15
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 11
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 7
	0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 3
};

/* TODO: libsterla should allocate a single GEM BO for kernels and
 * sub-allocate from there. gem_sync should take additional arguments,
 * for the sync range, which the underlying IOCTL already has. */

/* The simulator's bypass kernel copies every enabled column from its input to
 * its output, so a job is observable as data movement and not only as a fence
 * that signals. Three columns are bound and the fourth left empty, which must
 * be skipped rather than copied.
 *
 * Skipped on real hardware, where the CGRA computes whatever it was configured
 * to compute.
 *
 * This is also the only test that drives DRM_IOCTL_STRELA_GEM_SYNC around a
 * job, both ways: clean before submitting, invalidate once the fence signals.
 * The invalidate is what caught the allocation direction — it is an API
 * violation unless the buffer was allocated DMA_BIDIRECTIONAL, which is why the
 * driver allocates its own rather than calling drm_gem_dma_create(). Run this
 * against a CONFIG_DMA_API_DEBUG kernel or it proves nothing. */
static void test_data_is_copied(int fd)
{
	enum { N = 16, COLS = 3 };
	__u32 config, in[COLS], out[COLS], sync;
	__u32 *in_ptr[COLS], *out_ptr[COLS], *config_ptr;
	struct drm_strela_submit args = { 0 };
	int c, i;

	if (gem_new_mapped(fd, 80 * STRELA_WORD_SIZE, &config, (void **)&config_ptr)) {
		CHECK(false, "gem_new_mapped(config) failed: %s", strerror(errno));
		return;
	}
	memcpy(config_ptr, bypass_kernel_bitstream, sizeof bypass_kernel_bitstream);
	CHECK(gem_sync(fd, config, DRM_STRELA_SYNC_TO_DEVICE, 0,
			       sizeof bypass_kernel_bitstream) == 0,
		      "sync(config) failed: %s", strerror(errno));
	for (c = 0; c < COLS; c++) {
		if (gem_new_mapped(fd, N * STRELA_WORD_SIZE, &in[c],
				   (void **)&in_ptr[c]) ||
		    gem_new_mapped(fd, N * STRELA_WORD_SIZE, &out[c],
				   (void **)&out_ptr[c])) {
			CHECK(false, "buffer setup failed: %s", strerror(errno));
			return;
		}

		/* Distinct per column, so a crossed pair fails loudly. */
		for (i = 0; i < N; i++) {
			in_ptr[c][i] = 0xc0ffee00u + c * 0x100u + i;
			out_ptr[c][i] = 0xdeadbeefu;
		}

		CHECK(gem_sync(fd, in[c], DRM_STRELA_SYNC_TO_DEVICE, 0,
			       N * STRELA_WORD_SIZE) == 0,
		      "sync(in %d) failed: %s", c, strerror(errno));
	}

	sync = syncobj_new(fd);
	CHECK(sync != 0, "syncobj_new failed");

	args.config = (struct drm_strela_binding){
		.handle = config, .count = 80, .stride = 0,
	};
	args.out_syncobj = sync;
	for (c = 0; c < COLS; c++) {
		args.inputs[c] = (struct drm_strela_binding){
			.handle = in[c], .count = N, .stride = 1,
		};
		args.outputs[c] = (struct drm_strela_binding){
			.handle = out[c], .count = N, .stride = 0,
		};
	}
	/* Column 3 left zeroed on purpose. */

	CHECK(ioctl(fd, DRM_IOCTL_STRELA_SUBMIT, &args) == 0,
	      "submit failed: %s", strerror(errno));
	CHECK(syncobj_wait(fd, sync, 4000), "fence never signalled");
	CHECK(syncobj_status(fd, sync) > 0, "the job reported an error");
	syncobj_put(fd, sync);

	for (c = 0; c < COLS; c++) {
		/* The device wrote this buffer; drop whatever the CPU has
		 * cached for it before reading. */
		CHECK(gem_sync(fd, out[c], DRM_STRELA_SYNC_FROM_DEVICE, 0,
			       N * STRELA_WORD_SIZE) == 0,
		      "invalidate(out %d) failed: %s", c, strerror(errno));

		for (i = 0; i < N; i++)
			CHECK(out_ptr[c][i] == 0xc0ffee00u + c * 0x100u + i,
			      "column %d word %d is 0x%08x, expected 0x%08x",
			      c, i, out_ptr[c][i],
			      0xc0ffee00u + c * 0x100u + i);

		munmap(in_ptr[c], N * STRELA_WORD_SIZE);
		munmap(out_ptr[c], N * STRELA_WORD_SIZE);
	}
}

/* GEM_SYNC's own argument checking, which nothing else covers. A partial range
 * is the interesting case: the driver allows it deliberately, because the DMA
 * API lets a sync cover less than the mapping. */
static void test_gem_sync_args(int fd)
{
	const __u32 both = DRM_STRELA_SYNC_TO_DEVICE | DRM_STRELA_SYNC_FROM_DEVICE;
	__u32 handle;
	/* Exactly one page. GEM objects are page-granular — GEM_NEW rounds the
	 * requested size up — and the ioctl checks ranges against the object,
	 * so asking for less than a page would leave the tail of it legal and
	 * the out-of-range cases below would pass. */
	__u64 size = (__u64)sysconf(_SC_PAGESIZE);

	if (gem_new(fd, size, &handle)) {
		CHECK(false, "gem_new failed: %s", strerror(errno));
		return;
	}

	CHECK(gem_sync(fd, handle, both, 0, size) == 0,
	      "whole-buffer sync failed: %s", strerror(errno));
	CHECK(gem_sync(fd, handle, DRM_STRELA_SYNC_TO_DEVICE,
		       STRELA_WORD_SIZE, STRELA_WORD_SIZE) == 0,
	      "one-word sync failed: %s", strerror(errno));
	CHECK(gem_sync(fd, handle, 0, 0, size) == 0,
	      "sync with no direction failed: %s", strerror(errno));

	CHECK(gem_sync(fd, handle, 1u << 7, 0, size) < 0,
	      "sync accepted an unknown flag");
	CHECK(gem_sync(fd, handle, both, 0, size + 1) < 0,
	      "sync accepted a range past the end");
	CHECK(gem_sync(fd, handle, both, size, STRELA_WORD_SIZE) < 0,
	      "sync accepted an offset at the end");
	CHECK(gem_sync(fd, 0xdeadbeef, both, 0, size) < 0,
	      "sync accepted a bogus handle");

	gem_close(fd, handle);
}

/* Submit does not wait for the job: it queues it and returns. This is the whole
 * point of the rewrite, and the reason the old blocking ioctls are gone.
 *
 * Checked logically rather than by timing: with slow jobs, submit must return
 * while the fence is still unsignalled. A timing threshold cannot tell the two
 * apart under emulation, where an ioctl costs more than a job. */
static void test_submit_is_async(int fd)
{
	__u32 config, in, out, sync;

	CHECK(make_buffers(fd, &config, &in, &out) == 0, "gem_new failed");

	sync = submit(fd, config, in, out);
	CHECK(sync != 0, "submit failed: %s", strerror(errno));
	if (!sync)
		return;

	CHECK(!syncobj_wait(fd, sync, 0),
	      "fence already signalled when submit returned; submit is blocking?");
	CHECK(syncobj_wait(fd, sync, 4000), "job never finished");
	syncobj_put(fd, sync);
}

/* A job the hardware never finishes must not hang the driver: the timeout work
 * fails the fence with -ETIMEDOUT, and the queue keeps moving. */
static void test_timeout(int fd)
{
	__u32 config, in, out, sync, next;

	CHECK(make_buffers(fd, &config, &in, &out) == 0, "gem_new failed");

	sync = submit(fd, config, in, out);
	CHECK(sync != 0, "submit failed: %s", strerror(errno));
	if (!sync)
		return;

	CHECK(syncobj_wait(fd, sync, 4000),
	      "fence never signalled; a timed-out job must still signal");

	/* A wait cannot report this, so ask the fence itself. */
	CHECK(syncobj_status(fd, sync) < 0,
	      "fence reports status %d; expected an error", syncobj_status(fd, sync));

	syncobj_put(fd, sync);

	/* Every job times out in this configuration, so the check is that the
	 * device keeps accepting and retiring work rather than wedging. */
	next = submit(fd, config, in, out);
	CHECK(next != 0, "submit after a timeout failed: %s", strerror(errno));
	if (next) {
		CHECK(syncobj_wait(fd, next, 4000),
		      "device did not recover after a timeout");
		syncobj_put(fd, next);
	}
}

/* The device runs one job at a time, in submission order: when job k signals,
 * every earlier job has already signalled. Note this holds even though the
 * simulator jitters its delays. */
static void test_fifo_order(int fd)
{
	enum { N = 6 };
	__u32 config, in, out;
	__u32 sync[N];

	CHECK(make_buffers(fd, &config, &in, &out) == 0, "gem_new failed");

	for (int i = 0; i < N; i++) {
		sync[i] = submit(fd, config, in, out);
		CHECK(sync[i] != 0, "submit %d failed", i);
	}

	for (int i = 0; i < N; i++) {
		CHECK(syncobj_wait(fd, sync[i], 2000), "job %d never finished", i);

		/* Zero timeout: state right now, no waiting. */
		for (int j = 0; j < i; j++)
			CHECK(syncobj_wait(fd, sync[j], 0),
			      "job %d finished before job %d", i, j);
	}

	for (int i = 0; i < N; i++)
		syncobj_put(fd, sync[i]);
}

/* The kernel holds a reference to every buffer a queued job uses, so dropping
 * the userspace handle right after submitting is safe. Run this under KASAN:
 * without those references it is a use-after-free. */
static void test_buffers_outlive_handles(int fd)
{
	__u32 config, in, out, sync;

	CHECK(make_buffers(fd, &config, &in, &out) == 0, "gem_new failed");

	sync = submit(fd, config, in, out);
	CHECK(sync != 0, "submit failed: %s", strerror(errno));
	if (!sync)
		return;

	CHECK(gem_close(fd, config) == 0, "gem_close(config) failed");
	CHECK(gem_close(fd, in) == 0, "gem_close(in) failed");
	CHECK(gem_close(fd, out) == 0, "gem_close(out) failed");

	CHECK(syncobj_wait(fd, sync, 2000),
	      "job did not survive its handles being closed");
	syncobj_put(fd, sync);
}

/* A rejected submit returns an error, queues nothing, and leaks no fd. */
static void test_bad_handle(int fd)
{
	struct drm_strela_submit args = {
		.config = { .handle = 0xdeadbeef, .count = 80, .stride = 0 },
		.out_syncobj = syncobj_new(fd),
	};
	int before, after;

	/* Count open fds by probing for the lowest free one. */
	before = dup(0);
	close(before);

	CHECK(ioctl(fd, DRM_IOCTL_STRELA_SUBMIT, &args) < 0,
	      "submit with a bogus handle succeeded");
	CHECK(errno == ENOENT || errno == EINVAL, "unexpected errno %d (%s)",
	      errno, strerror(errno));

	after = dup(0);
	close(after);
	CHECK(before == after, "submit leaked an fd on the error path");
	syncobj_put(fd, args.out_syncobj);
}

/* Each device has its own queue: a busy device does not hold up another one.
 * Needs two devices and slow jobs, or the queue drains while it is being filled
 * — which is what happens under emulation, where an ioctl costs more than a
 * job. */
static void test_devices_are_independent(int fd0)
{
	__u32 config0, in0, out0, config1, in1, out1;
	__u32 sync0, sync1;
	unsigned int job_us;
	int fd1 = strela_open(1);

	if (fd1 < 0) {
		ksft_test_result_skip("devices are independent (needs two devices)\n");
		return;
	}

	if (!get_param("sim_job_delay_us", &job_us) || job_us < 10000) {
		ksft_test_result_skip("devices are independent (needs slow jobs)\n");
		close(fd1);
		return;
	}

	CHECK(make_buffers(fd0, &config0, &in0, &out0) == 0, "gem_new(0) failed");
	CHECK(make_buffers(fd1, &config1, &in1, &out1) == 0, "gem_new(1) failed");

	/* Give device 0 a backlog, then submit a single job to device 1. */
	for (int i = 0; i < 4; i++)
		syncobj_put(fd0, submit(fd0, config0, in0, out0));

	sync0 = submit(fd0, config0, in0, out0);
	sync1 = submit(fd1, config1, in1, out1);
	CHECK(sync0 && sync1, "submit failed");

	/* Device 1 has one job; device 0 has five. If the queues were shared,
	 * device 1's job could not finish first. */
	CHECK(syncobj_wait(fd1, sync1, 4000), "device 1 job never finished");
	CHECK(!syncobj_wait(fd0, sync0, 0),
	      "device 0 drained its whole queue first; shared queue?");

	CHECK(syncobj_wait(fd0, sync0, 4000), "device 0 job never finished");

	syncobj_put(fd0, sync0);
	syncobj_put(fd1, sync1);
	close(fd1);
}

/* Two processes submitting at once both make progress. The old driver refused
 * the second open() with EBUSY. */
static void test_two_processes(int fd)
{
	pid_t pid = fork();
	int status = 0;

	if (pid == 0) {
		int own_fd = strela_open(0);
		__u32 config, in, out, sync;

		if (own_fd < 0 || make_buffers(own_fd, &config, &in, &out))
			_exit(1);

		sync = submit(own_fd, config, in, out);
		_exit(sync && syncobj_wait(own_fd, sync, 4000) ? 0 : 1);
	}

	CHECK(pid > 0, "fork failed");
	test_single_job(fd);

	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "the other process could not use the device");
}

/* A job can wait for someone else's fence before it runs. Uses two devices, so
 * that without the dependency the second job would finish first: they have
 * independent queues. */
static void test_in_fence(int fd0)
{
	__u32 config0, in0, out0, config1, in1, out1;
	__u32 sync0, sync1, imported;
	long start_ms, elapsed_ms;
	unsigned int job_us, job_ms;
	int fd1 = strela_open(1);

	if (fd1 < 0) {
		ksft_test_result_skip("in-fence (needs two devices)\n");
		return;
	}

	if (!get_param("sim_job_delay_us", &job_us) || job_us < 10000) {
		ksft_test_result_skip("in-fence (needs slow simulated jobs)\n");
		close(fd1);
		return;
	}
	job_ms = job_us / 1000;

	CHECK(make_buffers(fd0, &config0, &in0, &out0) == 0, "gem_new(0) failed");
	CHECK(make_buffers(fd1, &config1, &in1, &out1) == 0, "gem_new(1) failed");

	sync0 = submit(fd0, config0, in0, out0);
	CHECK(sync0 != 0, "submit on device 0 failed: %s", strerror(errno));
	if (!sync0)
		goto out;

	/* Sync object handles are per open file, so the fence has to be exported
	 * from one device and imported into the other. */
	imported = syncobj_import(fd1, fd0, sync0);
	CHECK(imported != 0, "could not move the sync object between devices: %s",
	      strerror(errno));

	sync1 = syncobj_new(fd1);
	CHECK(submit_after(fd1, config1, in1, out1, sync1, imported) == 0,
	      "submit with an in-fence failed: %s", strerror(errno));

	/* Device 1 is idle, so only the dependency can hold this back. Both jobs
	 * take sim_job_delay_us, so with the dependency honoured the second
	 * cannot finish before two job durations have passed; without it, it
	 * would finish after one, alongside the first. Emulation only makes
	 * this slower, never faster, so the lower bound is safe. */
	start_ms = now_ms();
	CHECK(syncobj_wait(fd1, sync1, 8000), "the dependent job never ran");
	elapsed_ms = now_ms() - start_ms;

	CHECK(elapsed_ms > (long)(job_ms + job_ms / 2),
	      "the dependent job finished in %ldms; two jobs of %ums each should take longer",
	      elapsed_ms, job_ms);
	CHECK(syncobj_wait(fd0, sync0, 0),
	      "the dependent job finished before the job it waits on");

	syncobj_put(fd1, sync1);
	syncobj_put(fd1, imported);
	syncobj_put(fd0, sync0);
out:
	close(fd1);
}

/* A bad in-fence handle is rejected, and the flag is what makes the field
 * count: without it the field is ignored, which is how old userspace keeps
 * working. */
static void test_in_fence_bad_handle(int fd)
{
	struct drm_strela_submit args = {
		.flags = DRM_STRELA_SUBMIT_WAIT_SYNCOBJ,
		.in_syncobj = 0xdeadbeef,
	};
	__u32 config, in, out, sync;

	CHECK(make_buffers(fd, &config, &in, &out) == 0, "gem_new failed");
	args.out_syncobj = syncobj_new(fd);
	args.config = (struct drm_strela_binding){
		.handle = config, .count = 80, .stride = 0,
	};
	args.inputs[0] = (struct drm_strela_binding){
		.handle = in, .count = 16, .stride = 1,
	};
	args.outputs[0] = (struct drm_strela_binding){
		.handle = out, .count = 16, .stride = 0,
	};

	CHECK(ioctl(fd, DRM_IOCTL_STRELA_SUBMIT, &args) < 0,
	      "submit accepted a sync object that does not exist");
	CHECK(errno == ENOENT || errno == EINVAL, "unexpected errno %d (%s)",
	      errno, strerror(errno));
	syncobj_put(fd, args.out_syncobj);

	/* Same bogus handle, no flag: ignored. */
	sync = submit(fd, config, in, out);
	CHECK(sync != 0, "submit without the flag failed: %s", strerror(errno));
	if (sync) {
		CHECK(syncobj_wait(fd, sync, 8000), "job never finished");
		syncobj_put(fd, sync);
	}
}

/* Leaves jobs queued on purpose and returns without waiting for them. The
 * module is unloaded right after this suite, so the driver's drain and the
 * scheduler's teardown have to fail them: the harness fails the run if that
 * path warns, hangs or leaks. */
static void test_leave_work_queued(int fd)
{
	__u32 config, in, out;

	CHECK(make_buffers(fd, &config, &in, &out) == 0, "gem_new failed");

	for (int i = 0; i < 4; i++) {
		__u32 sync = submit(fd, config, in, out);

		/* Dropping the sync object leaves the job in flight with nobody
		 * waiting, which is the case teardown has to handle. */
		CHECK(sync != 0, "submit %d failed: %s", i, strerror(errno));
		syncobj_put(fd, sync);
	}
}


struct test {
	const char *name;
	void (*fn)(int fd);
};

/* Each suite needs the module loaded with different parameters, and parameters
 * are fixed at load time, so the harness (tests/qemu_init.c) reloads the module
 * between suites and passes the suite name here. */
static const struct test core_tests[] = {
	{ "single job completes", test_single_job },
	{ "a job copies its enabled columns", test_data_is_copied },
	{ "gem sync checks its arguments", test_gem_sync_args },
	{ "jobs complete in order", test_fifo_order },
	{ "buffers outlive their handles", test_buffers_outlive_handles },
	{ "bad handle is rejected cleanly", test_bad_handle },
	{ "two processes share a device", test_two_processes },
};

/* Needs slow jobs and shallow queues. */
static const struct test queue_tests[] = {
	{ "submit is asynchronous", test_submit_is_async },
	{ "devices are independent", test_devices_are_independent },
	{ "job waits for an in-fence", test_in_fence },
	{ "bad in-fence is rejected", test_in_fence_bad_handle },
	{ "work left in flight at unload", test_leave_work_queued },
};

/* Needs jobs longer than the driver's timeout. */
static const struct test timeout_tests[] = {
	{ "job timeout is survivable", test_timeout },
};

static const struct {
	const char *name;
	const struct test *tests;
	size_t count;
} suites[] = {
	{ "core", core_tests, ARRAY_SIZE(core_tests) },
	{ "queue", queue_tests, ARRAY_SIZE(queue_tests) },
	{ "timeout", timeout_tests, ARRAY_SIZE(timeout_tests) },
};

int main(int argc, char **argv)
{
	const char *wanted = argc > 1 ? argv[1] : "core";
	const struct test *tests = NULL;
	size_t count = 0;
	int fd;

	for (size_t i = 0; i < ARRAY_SIZE(suites); i++) {
		if (strcmp(suites[i].name, wanted) == 0) {
			tests = suites[i].tests;
			count = suites[i].count;
			break;
		}
	}

	if (!tests) {
		fprintf(stderr, "unknown suite '%s'\n", wanted);
		return 2;
	}

	fd = strela_open(0);

	ksft_print_header();
	ksft_set_plan(count);

	if (fd < 0) {
		ksft_exit_skip("no STRELA device found; is the module loaded?\n");
		return 1;
	}

	for (size_t i = 0; i < count; i++) {
		unsigned int before = ksft_test_num();

		failures = 0;
		tests[i].fn(fd);

		/* A test that reported its own result (a skip) is left alone. */
		if (ksft_test_num() != before)
			continue;

		if (failures)
			ksft_test_result_fail("%s\n", tests[i].name);
		else
			ksft_test_result_pass("%s\n", tests[i].name);
	}

	close(fd);
	ksft_finished();
}
