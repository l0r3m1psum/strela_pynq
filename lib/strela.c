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

struct strela_dev {
	int fd;

	unsigned tensors_bo_handle;
	void *tensors_bo_map;
	unsigned kernels_bo_handle;
	void *kernels_bo_map;

	unsigned syncobj_handle;
	struct drm_strela_submit submit;
	bool configured; /* implies initialized and not initialized implied not configured */

	bool initialized;
	strela_res res;
	Pool kernel_pool;
	Free_List buffer_free_list;
};

enum {
	STRELA_DEVICE_MAX = 8,
	STRELA_KERNEL_MEMORY_SIZE = sizeof (strela_word)*STRELA_KERNEL_SIZE*128,
	STRELA_TENSOR_MEMORY_SIZE = 1024*1024*50,
};

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
strela_device_count(unsigned *count) {
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

	if (!dev->initialized) {
		dev->res.errnum = STRELA_ERR_OK;

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
			dev->syncobj_handle = syncobj_create.handle;
			// FIXME: this needs to be out of band...
			pool_init(
				&dev->kernel_pool,
				kernels_pool_base,
				gem_new_kernels_args.size,
				sizeof (strela_word)*STRELA_KERNEL_SIZE,
				sizeof (strela_word)
			);
			// FIXME: this needs to be out of bound...
			free_list_init(
				&dev->buffer_free_list,
				tensors_free_list_base,
				gem_new_tensors_args.size
			);
		} else {
			assert(!dev->initialized);
			strela_dev_deinit_internal(
				tensors_free_list_base, STRELA_TENSOR_MEMORY_SIZE,
				kernels_pool_base, STRELA_KERNEL_MEMORY_SIZE,
				fd
			);
		}
	}

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
		// No need to free_all the allocators.
	}
	memset(dev, 0, sizeof *dev);
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

// TODO: do an out of band data structure for the pool allocator to be able to
// check if a handle is allocated or not and return STRELA_ERR_BAD_ARG if it is.
// struct { bool allocated; uint8_t next_free; } free_list[128];

void
strela_kernel_set(strela_dev *dev, strela_kernel kernel, const uint32_t data[STRELA_KERNEL_SIZE]) {
	if (strela_dev_ok(dev)) {
		if (!kernel.valid || kernel.handle >= 128) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}

		if (strela_dev_ok(dev)) {
			unsigned char *ptr = dev->kernel_pool.buf + kernel.handle * STRELA_KERNEL_SIZE * 4;
			memcpy(ptr, data, STRELA_KERNEL_SIZE * 4);
		}
	}
}

void
strela_kernel_free(strela_dev *dev, strela_kernel kernel) {
	if (strela_dev_ok(dev)) {
		if (!kernel.valid || kernel.handle >= 128) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
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
}

void
strela_kernel_free_all(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		pool_free_all(&dev->kernel_pool);
	}
}

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
			unsigned char *ptr = free_list_alloc(&dev->buffer_free_list, size_bytes, sizeof (strela_word));
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
		}
		free_list_free(&dev->buffer_free_list, strela_buffer_to_ptr(dev, buffer));
	}
}

void
strela_buffer_free_all(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		free_list_free_all(&dev->buffer_free_list);
	}
}

// TODO(Diego): just inline I guess...
static struct drm_strela_binding
strela_binding(__u32 handle, size_t offset, size_t count, size_t stride) {
	struct drm_strela_binding b = {
		.handle = handle,
		.offset = offset,
		.count = count,
		.stride = stride,
	};
	return b;
}

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
			.config = strela_binding(
				dev->kernels_bo_handle, kernel.handle * STRELA_KERNEL_SIZE, STRELA_KERNEL_SIZE, 0
			),
			.inputs = {
				strela_binding(tensors, conf->inp0_offset, conf->inp0_count, conf->inp0_stride),
				strela_binding(tensors, conf->inp1_offset, conf->inp1_count, conf->inp1_stride),
				strela_binding(tensors, conf->inp2_offset, conf->inp2_count, conf->inp2_stride),
				strela_binding(tensors, conf->inp3_offset, conf->inp3_count, conf->inp3_stride),
			},
			.outputs = {
				strela_binding(tensors, conf->out0_offset, conf->out0_count, 0),
				strela_binding(tensors, conf->out1_offset, conf->out1_count, 0),
				strela_binding(tensors, conf->out2_offset, conf->out2_count, 0),
				strela_binding(tensors, conf->out3_offset, conf->out3_count, 0),
			},
			.out_syncobj = dev->syncobj_handle,
		};

		dev->configured = true;
	}
}

/* TODO(Diego): this will be exposed pubblically via typed strela_buffer
 * interface and for kernels it will just be used internally. */
static void
strela_sync(strela_dev *dev, __u32 handle, __u32 flags, size_t offset_words, size_t words) {
	if (strela_dev_ok(dev) && words != 0) {
		/* TODO: allocations should be alligned by default! Here we round to
		 * page size just to be sure. */

		/* Round down and up respectivelly. */
		size_t start =   offset_words          * sizeof (strela_word)         & ~(size_t) 4095;
		size_t end   = ((offset_words + words) * sizeof (strela_word) + 4095) & ~(size_t) 4095;

		struct drm_strela_gem_sync args = {
			.handle = handle, .flags = flags, .offset = start, .length = end - start,
		};
		if (ioctl(dev->fd, DRM_IOCTL_STRELA_GEM_SYNC, &args) == -1) {
			perror("DRM_IOCTL_STRELA_GEM_SYNC");
			dev->res.errnum = errno;
		}
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
		/* Cache flush. */
		strela_sync(
			dev,
			dev->submit.config.handle,
			DRM_STRELA_SYNC_FINI | DRM_STRELA_SYNC_WRITE,
			dev->submit.config.offset,
			dev->submit.config.count
		);
		for (size_t i = 0; i < COUNTOF(dev->submit.inputs); i++) {
			const struct drm_strela_binding *in = &dev->submit.inputs[i];
			/* The last word read is at index (count - 1) * stride, so the
			 * span to flush is one word longer than that: a single word for
			 * stride 0. Without the test a count of 0 would wrap around. */
			/* TODO(Diego): use max instead. */
			size_t words = in->count == 0
				? 0 : (size_t) (in->count - 1) * in->stride + 1;
			strela_sync(
				dev, in->handle,
				DRM_STRELA_SYNC_FINI | DRM_STRELA_SYNC_WRITE,
				in->offset, words
			);
		}
	}

	if (strela_dev_ok(dev)) {
		if (ioctl(dev->fd, DRM_IOCTL_STRELA_SUBMIT, &dev->submit) == -1) {
			perror("DRM_IOCTL_STRELA_SUBMIT");
			dev->res.errnum = errno;
		}
	}

	if (strela_dev_ok(dev)) {
		if (ioctl_syncobj_wait(dev->fd, dev->syncobj_handle, 1000) == -1) {
			perror("DRM_IOCTL_SYNCOBJ_WAIT");
			dev->res.errnum = errno;
		}
	}

	if (strela_dev_ok(dev)) {
		int status = ioctl_syncobj_status(dev->fd, dev->syncobj_handle);
		if (status < 0) {
			errno = -status;
			perror("STRELA job failed");
			dev->res.errnum = -status;
		}
	}

	if (strela_dev_ok(dev)) {
		/* Cache invalidation. */
		for (size_t i = 0; i < COUNTOF(dev->submit.inputs); i++) {
			strela_sync(
				dev,
				dev->submit.outputs[i].handle,
				DRM_STRELA_SYNC_PREP | DRM_STRELA_SYNC_READ,
				dev->submit.outputs[i].offset,
				dev->submit.outputs[i].count
			);
		}
	}
}
