#include <assert.h>
#include <dirent.h>
#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "strela.h"
#include "strela_drm.h"

#include "allocators.c"

#define COUNTOF(x) (sizeof (x) / sizeof *(x))

enum {
	STRELA_DEVICE_MAX = 8,
	STRELA_JOBS_MAX = 16,
	STRELA_KERNEL_MEMORY_SIZE = sizeof (strela_word)*STRELA_KERNEL_SIZE*128,
	STRELA_TENSOR_MEMORY_SIZE = 1024*1024*50,
	CACHELINE_SIZE = 64,
};

enum strela_job_phase {
	STRELA_JOB_FREE,        /* slot not in use */
	STRELA_JOB_CONFIGURED,  /* submit is filled in, can be submitted */
	STRELA_JOB_SUBMITTED,   /* in the driver, fence_fd is valid */
	STRELA_JOB_DONE,        /* status is final, can be reconfigured or resubmitted */
};

struct strela_job_slot {
	enum strela_job_phase phase;

	/* What strela_config() builds today in dev->submit: the kernel and
	 * the eight column bindings. out_syncobj is this job's own. */
	struct drm_strela_submit submit;

	/* The fence of the current submit, exported as a sync file. This is
	 * the pollable fd, and SYNC_IOC_FILE_INFO on it gives the status.
	 * -1 unless SUBMITTED or DONE. */
	/* This is what ioctl_syncobj_status does internally with
	 * DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD*/
	int fence_fd;

	/* Valid when DONE: 1, or the job's negative errno. Per job, so a
	 * timeout does not touch the device's sticky error. */
	int status;
};

struct strela_dev {
	int fd;

	unsigned tensors_bo_handle;
	void *tensors_bo_map;
	unsigned kernels_bo_handle;
	void *kernels_bo_map;

	struct drm_strela_submit submit; // NOTE(Diego): will be moved to strela_job_slot
	bool configured; /* implies initialized and not initialized implies not configured */

	bool initialized;
	strela_res res;
	Pool kernel_pool;
	Free_List buffer_free_list;

	struct strela_job_slot jobs[STRELA_JOBS_MAX];
};

static bool
is_aligned(uintptr_t ptr, size_t align) { return ptr % align == 0; }

/* The kernel has a limit of ACCEL_MAX_MINORS shared with other kind of
 * accelerators. Counting them and opening the n-th, ignoring the unavoidable
 * TOCTOU preoblems, needs to do a linear search in /dev/accel.
 */
static strela_dev devices[STRELA_DEVICE_MAX];

typedef struct accel_fd accel_fd;
struct accel_fd {
	int fd;
	char drm_name[16];
};

static accel_fd
accel_openat(int dirfd, const char *name) {
	// NOTE(Diego): Here it is fine but in general FDs should be initialized to -1
	accel_fd fd = {0};

	fd.fd = openat(dirfd, name, O_RDWR | O_CLOEXEC);
	if (fd.fd == -1) {
		perror("openat");
	}

	/* The kernel does not zero terminate. We set the buffer length to one less
	 * of its actual size to guarantee it. */
	struct drm_version version = {
		.name = fd.drm_name,
		.name_len = sizeof fd.drm_name - 1,
	};

	if (fd.fd != -1 && ioctl(fd.fd, DRM_IOCTL_VERSION, &version) == -1) {
		perror("DRM_IOCTL_VERSION");
		close(fd.fd);
		fd.fd = -1;
	}

	bool has_been_truncated = version.name_len > sizeof fd.drm_name - 1;
	if (has_been_truncated) {
		// TODO: maybe emit warning.
	}

	return fd;
}

typedef struct accel_iter accel_iter;
struct accel_iter {
	DIR *dir;
	int myerrno; /* Stupid errno macro... */
	bool valid;
	const char *ent_name;
};
/* TODO: write an invariant for accel_iter. */

static void
accel_iter_next(accel_iter *iter);

/* TODO: The iterator should take a DIR * as an argument wich shuold be closed
 * outside the loop
 * dir = opendir();
 * for () {}
 * closedir(dir); */
static accel_iter
accel_iter_make() {
	accel_iter iter = {0};
	iter.dir = opendir("/dev/accel");
	bool valid = true;
	if (!iter.dir) {
		valid = false;
		if (errno != ENOENT) {
			perror("opendir(\"/dev/accel\")");
			iter.myerrno = errno;
		} else {
			/* No directory at all means no accel devices, which is not an
			 * error. */
			iter.myerrno = 0;
		}
	}
	iter.valid = valid;

	/* Finishes the initialzation. */
	accel_iter_next(&iter);

	return iter;
}

static bool
accel_iter_is_valid(accel_iter *iter) {
	return iter->valid;
}

static void
accel_iter_close(accel_iter *iter) {
	if (iter->dir) {
		closedir(iter->dir);
		iter->dir = NULL;
		iter->valid = 0;
		iter->ent_name = NULL;
	}
}

static void
accel_iter_next(accel_iter *iter) {
	iter->valid = 0;
	while (iter->dir) {
		errno = 0;
		struct dirent *entry = readdir(iter->dir);
		if (!entry) {
			iter->myerrno = errno;
			if (errno) {
				perror("readdir");
			}
			accel_iter_close(iter);
			break;
		}

		/* Skips '.'' and '..', /dev/accel should only contains back-links and
		 * accel files. */
		if(entry->d_name[0] == '.') {
			continue;
		}

		iter->valid = true;
		iter->ent_name = entry->d_name;
		break;
	}
}

int
strela_dev_count(unsigned *count) {
	*count = 0;

	accel_iter iter = accel_iter_make();
	for (;
		accel_iter_is_valid(&iter);
		accel_iter_next(&iter)) {

		accel_fd fd = accel_openat(dirfd(iter.dir), iter.ent_name);

		if (fd.fd != -1) {
			if (strcmp(STRELA_DRM_NAME, fd.drm_name) == 0) {
				(*count)++;
			}
			close(fd.fd);
		}
	}

	return iter.myerrno ? -1 : 0;
}

void
strela_dev_reset_err(strela_dev *dev) {
	if (dev->initialized) {
		dev->res.errnum = STRELA_ERR_OK;
	}
}

strela_res
strela_dev_get_err(strela_dev *dev) {
	return dev->res;
}

bool
strela_dev_initialized(strela_dev *dev) {
	return dev->initialized;
}

bool
strela_dev_ok(strela_dev *dev) {
	return dev->initialized && dev->res.errnum == STRELA_ERR_OK;
}

/* Kernel submodule ***********************************************************/

strela_kernel
strela_kernel_alloc(strela_dev *dev) {
	/* memset and not = {0}: an initializer need not zero the padding, and
	 * handles are meant to be comparable with memcmp. */
	strela_kernel res;
	memset(&res, 0, sizeof res);
	if (strela_dev_ok(dev)) {
		unsigned char *ptr = pool_alloc(&dev->kernel_pool);
		if (ptr) {
			unsigned handle = ((uintptr_t) ptr - (uintptr_t) dev->kernel_pool.buf)
				/ ((uintptr_t) STRELA_KERNEL_SIZE * (uintptr_t) 4);
			assert(handle < 128);
			res.valid = true;
			res.handle = handle;
		} else {
			dev->res.errnum = -STRELA_ERR_NO_MEM;
		}
	}
	return res;
}

static void
strela_sync(strela_dev *dev, __u32 handle, __u32 flags, size_t offset_words, size_t words) {
	if (strela_dev_ok(dev) && words != 0) {

		struct drm_strela_gem_sync args = {
			.handle = handle, .flags = flags, .offset = offset_words*sizeof(strela_word), .length = words*sizeof(strela_word),
		};
		if (ioctl(dev->fd, DRM_IOCTL_STRELA_GEM_SYNC, &args) == -1) {
			perror("DRM_IOCTL_STRELA_GEM_SYNC");
			dev->res.errnum = errno;
		}
	}
}

// TODO: do an out of band data structure for the pool allocator to be able to
// check if a handle is allocated or not and return STRELA_ERR_BAD_ARG if it is.
// struct { bool allocated; uint8_t next_free; } free_list[128];

void
strela_kernel_set(strela_dev *dev, strela_kernel kernel, const uint32_t data[STRELA_KERNEL_SIZE]) {
	if (strela_dev_ok(dev)) {
		if (!kernel.valid || kernel.handle >= 128) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	if (strela_dev_ok(dev)) {
		unsigned char *ptr = dev->kernel_pool.buf + kernel.handle * STRELA_KERNEL_SIZE * 4;
		memcpy(ptr, data, STRELA_KERNEL_SIZE * 4);
		strela_sync(
			dev,
			dev->kernels_bo_handle,
			DRM_STRELA_SYNC_FINI | DRM_STRELA_SYNC_WRITE,
			kernel.handle*STRELA_KERNEL_SIZE,
			STRELA_KERNEL_SIZE
		);
	}
}

void
strela_kernel_free(strela_dev *dev, strela_kernel kernel) {
	if (strela_dev_ok(dev)) {
		if (!kernel.valid || kernel.handle >= 128) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	if (strela_dev_ok(dev)) {
		unsigned char *ptr = dev->kernel_pool.buf + kernel.handle * STRELA_KERNEL_SIZE * 4;
		if (false) {
			pool_free(&dev->kernel_pool, ptr);
		} else {
			fprintf(stderr, "Kernel freeing is not implemented right now "
				"because separate configuration and execution is not "
				"supported yet. If you want to free memory you need to call"
				"strela_kernel_free_all\n");
		}
	}
}

void
strela_kernel_free_all(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		pool_free_all(&dev->kernel_pool);
	}
}

/* Buffer submodule ***********************************************************/

static size_t
internal_ptr_to_buffer_offset(strela_dev *dev, const void *ptr) {
	if (!strela_dev_ok(dev)) abort();
	return ((uintptr_t) ptr - (uintptr_t) dev->tensors_bo_map)
		/ (uintptr_t) sizeof (strela_word);
}

strela_buffer
strela_buffer_alloc(strela_dev *dev, size_t size_words) {
	/* See strela_kernel_alloc. */
	strela_buffer res;
	memset(&res, 0, sizeof res);
	if (strela_dev_ok(dev)) {
		size_t size_bytes = size_words * sizeof (strela_word);
		if (size_bytes >= size_words) {
			/* The minimal unit for cache synchornization (i.e flushing and
			 * invalidation) should be one cache line (usually 64 byte). */
			size_t page_allignment = 64;
			unsigned char *ptr = free_list_alloc(
				&dev->buffer_free_list, size_bytes, page_allignment
			);
			if (ptr) {
				res.valid = true;
				res.size_words = size_words;
				res.offset_words_from_base = internal_ptr_to_buffer_offset(dev, ptr);
			} else {
				dev->res.errnum = -STRELA_ERR_NO_MEM;
			}
		} else {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}
	return res;
}

strela_word *
strela_buffer_to_ptr(strela_dev *dev, strela_buffer buffer) {
	// We do not want to return null pointers and requiring that this function
	// is used only when no error has ever happened is a reasonable request for
	// the programmer.
	if (!buffer.valid || !strela_dev_ok(dev)) abort();
	return (strela_word *) (
		(uintptr_t) dev->tensors_bo_map
		+ (uintptr_t) (buffer.offset_words_from_base*sizeof (strela_word))
	);
}

strela_buffer
strela_buffer_from_ptr(strela_dev *dev, const void *ptr) {
	/* See strela_kernel_alloc. */
	strela_buffer res;
	memset(&res, 0, sizeof res);
	if (strela_dev_ok(dev)) {
		if ((uintptr_t) dev->tensors_bo_map <= (uintptr_t) ptr
			&& (uintptr_t) ptr < (uintptr_t) dev->tensors_bo_map + STRELA_TENSOR_MEMORY_SIZE) {
			res.valid = true;
			res.offset_words_from_base = internal_ptr_to_buffer_offset(dev, ptr);
			res.size_words = (
				(Free_List_Allocation_Header *)
				((uintptr_t) ptr - sizeof (Free_List_Allocation_Header))
			)->orig_size;
			// We could also check that the pointer is not part of the free list
			// but assuming that the allocation is valid this is the best we can
			// do.
			if (res.size_words % sizeof (strela_word) != 0) abort();
			res.size_words /= sizeof (strela_word);
		}
	}
	return res;
}

void
strela_buffer_set_data(strela_dev *dev, strela_buffer buffer, const strela_word *ptr) {
	if (strela_dev_ok(dev)) {
		if (!buffer.valid) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		} else {
			strela_word *strela_buf = strela_buffer_to_ptr(dev, buffer);
			memcpy(strela_buf, ptr, buffer.size_words*sizeof (strela_word));
		}
	}
}

void
strela_buffer_get_data(strela_dev *dev, strela_buffer buffer, strela_word *ptr) {
	if (strela_dev_ok(dev)) {
		if (!buffer.valid) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		} else {
			const strela_word *strela_buf = strela_buffer_to_ptr(dev, buffer);
			memcpy(ptr, strela_buf, buffer.size_words*sizeof (strela_word));
		}
	}
}

void
strela_buffer_free(strela_dev *dev, strela_buffer buffer) {
	if (strela_dev_ok(dev)) {
		if (!buffer.valid) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		} else {
			free_list_free(&dev->buffer_free_list, strela_buffer_to_ptr(dev, buffer));
		}
	}
}

void
strela_buffer_free_all(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		free_list_free_all(&dev->buffer_free_list);
	}
}

static void
strela_buffer_sync(
	strela_dev *dev, strela_buffer buffer, __u32 flags, size_t offset, size_t count
) {
	size_t total = STRELA_TENSOR_MEMORY_SIZE / sizeof(strela_word);
	if (strela_dev_ok(dev)) {
		if (
			!buffer.valid
			|| offset > buffer.size_words
			|| count > buffer.size_words - offset
			|| buffer.offset_words_from_base > total
			|| buffer.size_words > total - buffer.offset_words_from_base
		) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	if (strela_dev_ok(dev)) {
		strela_sync(dev, dev->tensors_bo_handle, flags, buffer.offset_words_from_base + offset, count);
	}
}

void
strela_buffer_flush(
	strela_dev *dev, strela_buffer buffer, size_t offset, size_t count
) {
	enum { CACHELINE_WORDS = CACHELINE_SIZE / sizeof (strela_word) };

	/* Round down and up respectively. */
	size_t mask  = ~(size_t) (CACHELINE_WORDS - 1);
	size_t start = offset & mask;
	size_t end   = (offset + count + CACHELINE_WORDS - 1) & mask;

	if (count != 0) {
		strela_buffer_sync(
			dev, buffer, DRM_STRELA_SYNC_FINI | DRM_STRELA_SYNC_WRITE, start, end - start /* i.e. count */
		);
	}
}

/* Invalidate marks the cache line as empty without writing its contents back to
 * memory. If the line held data the CPU had written but that had not reached
 * memory yet, that data is gone. Hence it is hazardous to invelidate memory of
 * a neighbour. As in Vulkan's vkInvalidateMappedMemoryRanges we disallow this.
 */
void
strela_buffer_inval(
	strela_dev *dev, strela_buffer buffer, size_t offset, size_t count
) {
	if (   !is_aligned(offset * sizeof (strela_word), CACHELINE_SIZE)
		|| !is_aligned(count  * sizeof (strela_word), CACHELINE_SIZE)
	) {
		dev->res.errnum = -STRELA_ERR_BAD_ARG;
	}

	strela_buffer_sync(
		dev, buffer, DRM_STRELA_SYNC_PREP | DRM_STRELA_SYNC_READ, offset, count
	);
}

/*Old Execution submodule *****************************************************/

void
strela_config(strela_dev *dev, strela_kernel kernel, strela_conf *conf) {
	if (strela_dev_ok(dev)) {
		if (!kernel.valid || kernel.handle >= 128) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	if (strela_dev_ok(dev)) {
		__u32 tensors = dev->tensors_bo_handle;

		dev->submit = (struct drm_strela_submit) {
			.config = {
				.handle = dev->kernels_bo_handle,
				.offset = kernel.handle * STRELA_KERNEL_SIZE,
				.count = STRELA_KERNEL_SIZE,
			},
			.inputs = {
				{ tensors, conf->inp0_offset, conf->inp0_count, conf->inp0_stride },
				{ tensors, conf->inp1_offset, conf->inp1_count, conf->inp1_stride },
				{ tensors, conf->inp2_offset, conf->inp2_count, conf->inp2_stride },
				{ tensors, conf->inp3_offset, conf->inp3_count, conf->inp3_stride },
			},
			.outputs = {
				{ tensors, conf->out0_offset, conf->out0_count },
				{ tensors, conf->out1_offset, conf->out1_count },
				{ tensors, conf->out2_offset, conf->out2_count },
				{ tensors, conf->out3_offset, conf->out3_count },
			},
			/* Created by strela_dev_init(); kept across configurations. */
			.out_syncobj = dev->submit.out_syncobj,
		};

		dev->configured = true;
	}
}

static int
ioctl_syncobj_wait(int fd, __u32 handle, int timeout_ms)
{
	struct timespec ts = { 0 };
	struct drm_syncobj_wait args = {
		.handles = (__u64)(uintptr_t)&handle,
		.count_handles = 1,
		.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
	};

	clock_gettime(CLOCK_MONOTONIC, &ts);
	args.timeout_nsec = (__s64)ts.tv_sec * 1000000000 + ts.tv_nsec +
			    (__s64)timeout_ms * 1000000;

	return ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);
}

/* A wait only says the fence signalled, not whether the job failed. Returns 1
 * if it completed, 0 if it is still pending, or a negative errno: the job's
 * own (-ECANCELED after a timeout) or that of a failed ioctl. */
/* NOTE(Diego): this API is so shit but it is the correct usage... */
static int
ioctl_syncobj_status(int fd, __u32 handle)
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

	if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export) == -1)
		return -errno;

	ret = ioctl(export.fd, SYNC_IOC_FILE_INFO, &info);
	if (ret == -1)
		ret = -errno;
	else
		ret = fence_info.status;
	close(export.fd);

	return ret;
}

void
strela_execute(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		if (!dev->configured) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	if (strela_dev_ok(dev)) {
		if (ioctl(dev->fd, DRM_IOCTL_STRELA_SUBMIT, &dev->submit) == -1) {
			perror("DRM_IOCTL_STRELA_SUBMIT");
			dev->res.errnum = errno;
		}
	}

	if (strela_dev_ok(dev)) {
		if (ioctl_syncobj_wait(dev->fd, dev->submit.out_syncobj, 1000) == -1) {
			perror("DRM_IOCTL_SYNCOBJ_WAIT");
			dev->res.errnum = errno;
		}
	}

	if (strela_dev_ok(dev)) {
		int status = ioctl_syncobj_status(dev->fd, dev->submit.out_syncobj);
		if (status < 0) {
			errno = -status;
			perror("STRELA job failed");
			dev->res.errnum = -status;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// TODO(Diego): job free should wait for them to finish...
// TODO(Diego): we can use handle 0 as always an invalid job....
////////////////////////////////////////////////////////////////////////////////

/* Job summodule **************************************************************/

static void
strela_job_slot_release(strela_dev *dev, struct strela_job_slot *slot) {
	assert(strela_dev_initialized(dev));
	/* This is because the slot->fence_fd is initialized to 0 by
	 * memset(dev->jobs, 0, sizeof dev->jobs) in strela_dev_init leaves FDs to 0.
	 */
	if (slot->phase != STRELA_JOB_FREE) {
		if (slot->fence_fd != -1) {
			if (close(slot->fence_fd) == -1) {
				perror("close");
				abort();
			}
		}
		if (slot->submit.out_syncobj != 0) {
			struct drm_syncobj_destroy args = { .handle = slot->submit.out_syncobj };
			if (ioctl(dev->fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args) == -1) {
				perror("DRM_IOCTL_SYNCOBJ_DESTROY");
				abort();
			}
		}
		memset(slot, 0, sizeof *slot);
		slot->fence_fd = -1;
		slot->phase = STRELA_JOB_FREE;
	}
}

void
strela_job_free_all(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		for (size_t i = 0; i < COUNTOF(dev->jobs); i++) {
			struct strela_job_slot *slot = &dev->jobs[i];
			// NOTE(Diego): check that the invariant we maintain allows for this.
			strela_job_slot_release(dev, slot);
		}
	}
}

strela_job
strela_job_config(strela_dev *dev, strela_kernel kernel, strela_conf *conf) {
	strela_job res = {0};

	if (strela_dev_ok(dev)) {
		if (!kernel.valid) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	size_t i = 0;
	struct strela_job_slot *job = NULL;
	if (strela_dev_ok(dev)) {
		/* There are better ways of doing this, but this works... */
		for (; i < COUNTOF(dev->jobs); i++) {
			if (dev->jobs[i].phase == STRELA_JOB_FREE) {
				job = &dev->jobs[i];
				break;
			}
		}
	}

	if (strela_dev_ok(dev)) {
		if (!job) {
			dev->res.errnum = -STRELA_ERR_NO_MEM;
		}
	}

	struct drm_syncobj_create syncobj_create = { 0 };
	if (strela_dev_ok(dev)) {
		if (ioctl(dev->fd, DRM_IOCTL_SYNCOBJ_CREATE, &syncobj_create) == -1) {
			perror("DRM_IOCTL_SYNCOBJ_CREATE");
			dev->res.errnum = errno;
		}
	}

	if (strela_dev_ok(dev)) {
		__u32 tensors = dev->tensors_bo_handle;

		job->phase = STRELA_JOB_CONFIGURED;
		job->fence_fd = -1;
		job->submit = (struct drm_strela_submit) {
			.config = {
				.handle = dev->kernels_bo_handle,
				.offset = kernel.handle * STRELA_KERNEL_SIZE,
				.count = STRELA_KERNEL_SIZE,
			},
			.inputs = {
				{ tensors, conf->inp0_offset, conf->inp0_count, conf->inp0_stride },
				{ tensors, conf->inp1_offset, conf->inp1_count, conf->inp1_stride },
				{ tensors, conf->inp2_offset, conf->inp2_count, conf->inp2_stride },
				{ tensors, conf->inp3_offset, conf->inp3_count, conf->inp3_stride },
			},
			.outputs = {
				{ tensors, conf->out0_offset, conf->out0_count },
				{ tensors, conf->out1_offset, conf->out1_count },
				{ tensors, conf->out2_offset, conf->out2_count },
				{ tensors, conf->out3_offset, conf->out3_count },
			},
			.out_syncobj = syncobj_create.handle,
		};

		res.valid = true;
		res.handle = (unsigned) i;
	} else {
		if (job) {
			strela_job_slot_release(dev, job);
		}
	}

	return res;
}

#define IN_BOUND(i, a) ((i) >= 0 && (i) < COUNTOF(a))

void
strela_job_free(strela_dev *dev, strela_job job) {
	if (strela_dev_ok(dev)) {
		if (!job.valid) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		} else if (!IN_BOUND(job.handle, dev->jobs)) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	if (strela_dev_ok(dev)) {
		struct strela_job_slot *slot = &dev->jobs[job.handle];
		strela_job_slot_release(dev, slot);
	}
}

bool
strela_job_submit(strela_dev *dev, strela_job job) {
	if (strela_dev_ok(dev)) {
		if (!job.valid) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		} else if (!IN_BOUND(job.handle, dev->jobs)) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	struct strela_job_slot *slot = &dev->jobs[job.handle];
	if (strela_dev_ok(dev)) {
		if (slot->phase != STRELA_JOB_CONFIGURED) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
	}

	if (strela_dev_ok(dev)) {
		if (ioctl(dev->fd, DRM_IOCTL_STRELA_SUBMIT, &slot->submit) == -1) {
			perror("DRM_IOCTL_STRELA_SUBMIT");
			dev->res.errnum = errno;
		}
	}

	if (strela_dev_ok(dev)) {
		/* The job is in the driver from here on, whatever the export does. */
		slot->phase = STRELA_JOB_SUBMITTED;

		struct drm_syncobj_handle export = {
			.handle = slot->submit.out_syncobj,
			.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
			.fd = -1,
		};
		if (ioctl(dev->fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &export) == -1) {
			perror("DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD");
			dev->res.errnum = errno;
		} else {
			slot->fence_fd = export.fd;
		}
	}

	// TODO(Diego): right now we always return false, mixing monadic sticky
	// errors with returns is a bit of a PITA.
	return false;
}

strela_job_state
strela_job_poll(strela_dev *dev, strela_job job) {
	abort();
	return STRELA_JOB_FAILED;
}

bool
strela_job_wait(strela_dev *dev, strela_job job, int timeout_ms) {
	abort();
	return false;
}

int
strela_job_fd(strela_dev *dev, strela_job job) {
	abort();
	return -1;
}

/* Device submodule ***********************************************************/

static void
strela_dev_deinit_internal(
	void *tensors_base, size_t tensors_size,
	void *kernels_base, size_t kernels_size,
	int device_fd
) {
	/* Closing the fd after removing the mappings releases both buffer handles.
	 */
	const char *munmap_err_msg = "munmap(2) can only fail because it was passed"
		" bad arguments (EINVAL). This is non recoverable programmer error.";
	if (tensors_base != NULL && tensors_base != MAP_FAILED) {
		if (munmap(tensors_base, tensors_size) == -1) {
			perror(munmap_err_msg);
			abort();
		}
	}
	if (kernels_base != NULL && kernels_base != MAP_FAILED) {
		if (munmap(kernels_base, kernels_size) == -1) {
			perror(munmap_err_msg);
			abort();
		}
	}
	/* TODO(Diego): in twothoussand-never handle EINTR. */
	if (device_fd != -1) {
		if (close(device_fd) == -1) {
			perror("close(2) can only fail because it was passed a bad argument"
				"(EINVAL). This is a not recoverable programmer error.");
			abort();
		}
	}
}

strela_dev *
strela_dev_init(unsigned which_strela) {
	if (which_strela >= STRELA_DEVICE_MAX) {
		/* We consider this an unrecoverable programmer error. Otherwise it
		 * becomes cumbersome to handle this corner case. */
		abort();
	}

	/* We have to pick this and not the accel file index because if it is not
	 * present we need to return an error.  */
	strela_dev *dev = &devices[which_strela];

	dev->res.errnum = STRELA_ERR_OK;

	if (!dev->initialized) {
		int fd = -1;
		{
			unsigned count = 0;
			for (accel_iter iter = accel_iter_make();
				accel_iter_is_valid(&iter);
				accel_iter_next(&iter)) {

				accel_fd afd = accel_openat(dirfd(iter.dir), iter.ent_name);

				if (afd.fd != -1) {
					if (strcmp(STRELA_DRM_NAME, afd.drm_name) == 0) {
						if (count++ == which_strela) {
							fd = afd.fd;
							accel_iter_close(&iter);
							break;
						}
					}
					close(afd.fd);
				}
			}
		}

		if (fd == -1) {
			errno = ENODEV;
			perror("STRELA device not found");
			dev->res.errnum = errno;
		}

		// TODO: rename DRM_IOCTL_STRELA_GEM_NEW to DRM_IOCTL_STRELA_CREATE_BO
		// to have a similar name to the Rocket driver.

		struct drm_strela_gem_new gem_new_kernels_args = {
			.size = STRELA_KERNEL_MEMORY_SIZE
		};
		if (dev->res.errnum == STRELA_ERR_OK) {
			if (ioctl(fd, DRM_IOCTL_STRELA_GEM_NEW, &gem_new_kernels_args) == -1) {
				perror("DRM_IOCTL_STRELA_GEM_NEW");
				dev->res.errnum = errno;
			}
		}

		void *kernels_pool_base = NULL;
		if (dev->res.errnum == STRELA_ERR_OK) {
			kernels_pool_base = mmap(
				NULL, gem_new_kernels_args.size, PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, gem_new_kernels_args.offset);
			if (kernels_pool_base == MAP_FAILED) {
				perror("mmap");
				dev->res.errnum = errno;
			}
		}

		struct drm_strela_gem_new gem_new_tensors_args = {
			.size = STRELA_TENSOR_MEMORY_SIZE,
		};
		if (dev->res.errnum == STRELA_ERR_OK) {
			if (ioctl(fd, DRM_IOCTL_STRELA_GEM_NEW, &gem_new_tensors_args) == -1) {
				perror("DRM_IOCTL_STRELA_GEM_NEW");
				dev->res.errnum = errno;
			}
		}

		void *tensors_free_list_base = NULL;
		if (dev->res.errnum == STRELA_ERR_OK) {
			tensors_free_list_base = mmap(
				NULL, gem_new_tensors_args.size, PROT_READ | PROT_WRITE,
				MAP_SHARED, fd, gem_new_tensors_args.offset
			);
			if (tensors_free_list_base == MAP_FAILED) {
				perror("mmap");
				dev->res.errnum = errno;
			}
		}

		struct drm_syncobj_create syncobj_create = { 0 };
		if (dev->res.errnum == STRELA_ERR_OK) {
			if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &syncobj_create) == -1) {
				perror("DRM_IOCTL_SYNCOBJ_CREATE");
				dev->res.errnum = errno;
			}
		}

		if (dev->res.errnum == STRELA_ERR_OK) {
			dev->fd = fd;
			dev->initialized = true;
			dev->tensors_bo_handle = gem_new_tensors_args.handle;
			dev->tensors_bo_map = tensors_free_list_base;
			dev->kernels_bo_handle = gem_new_kernels_args.handle;
			dev->kernels_bo_map = kernels_pool_base;
			dev->configured = false;
			dev->submit.out_syncobj = syncobj_create.handle;
			pool_init(
				&dev->kernel_pool,
				dev->kernels_bo_map,
				STRELA_KERNEL_MEMORY_SIZE,
				sizeof (strela_word)*STRELA_KERNEL_SIZE,
				sizeof (strela_word)
			);
			free_list_init(
				&dev->buffer_free_list,
				dev->tensors_bo_map,
				STRELA_TENSOR_MEMORY_SIZE
			);
			/* This should not be needed bu we do it just in case... */
			memset(dev->jobs, 0, sizeof dev->jobs);
		} else {
			assert(!dev->initialized);
			strela_dev_deinit_internal(
				tensors_free_list_base, STRELA_TENSOR_MEMORY_SIZE,
				kernels_pool_base, STRELA_KERNEL_MEMORY_SIZE,
				fd
			);
		}
	}

	if (dev->initialized) {
		assert(dev->res.errnum == STRELA_ERR_OK);
		strela_job_free_all(dev);
		/* There is a bit of work duplication with pool_init nad free_list_init,
		 * the compiler should remove it but a better architecture would be nice.
		 */
		strela_buffer_free_all(dev);
		strela_kernel_free_all(dev);
	}

	/* The device has an error iff it is not initialized. */
	assert((bool) dev->res.errnum == (bool) !dev->initialized);

	return dev;
}

void
strela_dev_deinit(strela_dev *dev) {
	if (dev->initialized) {
		strela_dev_deinit_internal(
			dev->tensors_bo_map, STRELA_TENSOR_MEMORY_SIZE,
			dev->kernels_bo_map, STRELA_KERNEL_MEMORY_SIZE,
			dev->fd
		);
		/* No need to free_all the allocators. */
	}
	memset(dev, 0, sizeof *dev);
}
