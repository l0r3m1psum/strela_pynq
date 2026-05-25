#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>

#include "strela.h"
#include "strela_ioctl.h"

#include "allocators.c"

#define COUNTOF(x) (sizeof (x) / sizeof *(x))

struct strela_dev {
	int fd;
	void *base;

	bool initialized;
	strela_res res;
	Pool kernel_pool;
	Arena buffer_arena;
};

static strela_dev devices[STRELA_MAX_NUM];

int
strela_device_count(unsigned *count) {
	int local_count = 0;
	char path_buf[] = "/dev/strelaX";
	int last_char = COUNTOF(path_buf) - 1 - 1;
	int res = 0;
	int old_errno = 0;

	// TODO: use snprintf
	static_assert(STRELA_MAX_NUM < 10, "maximum one decimal digit.");
	// We need to rest errno to determine if access failed or the file just does
	// not exists.
	old_errno = errno;
	errno = 0;

	for (int i = 0; i < STRELA_MAX_NUM; i++) {
		path_buf[last_char] = '0' + i;
		if (access(path_buf, F_OK) == -1) {
			break;
		} else {
			local_count++;
		}
	}

	*count = local_count;

	if (errno != 0) {
		perror("access");
		res = -1;
	} else {
		errno = old_errno;
	}

	return res;
}

bool
strela_dev_ok(strela_dev *dev) {
	return dev->initialized && dev->res.errnum == STRELA_ERR_OK;
}

strela_dev *
strela_dev_init(unsigned which_strela) {
	char path_buf[128] = {0};
	int ret = 0, fd = -1;
	void *base = NULL;
	strela_dev *dev = NULL;

	if (which_strela >= STRELA_MAX_NUM) {
		// We consider this an unrecoverable programmer error. Otherwise it
		// becomes cumbersome to handle this corner case.
		abort();
	}

	dev = &devices[which_strela];

	if (!dev->initialized) {
		ret = snprintf(path_buf, sizeof path_buf, "/dev/strela%d", which_strela);
		assert(ret >= 0);

		fd = open(path_buf, O_RDWR);
		if (fd == -1) {
			perror("open");
			dev->res.errnum = errno;
		}

		if (dev->res.errnum == STRELA_ERR_OK) {
			base = mmap(NULL, STRELA_DATA_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (base == MAP_FAILED) {
				perror("mmap");
				dev->res.errnum = errno;
			}
		}

		if (dev->res.errnum == STRELA_ERR_OK) {
			dev->fd = fd;
			dev->base = base;
			dev->initialized = true;
			pool_init(
				&dev->kernel_pool,
				base,
				4*STRELA_KERNEL_SIZE*128,
				4*STRELA_KERNEL_SIZE,
				1 // Kernels can be byte aligned.
			);
			arena_init(
				&dev->buffer_arena,
				(unsigned char *) base + 4*STRELA_KERNEL_SIZE*128,
				STRELA_DATA_REGION_SIZE - 4*STRELA_KERNEL_SIZE*128
			);
		} else {
			assert(!dev->initialized);
			if (fd != -1) close(fd);
		}
	}

	return dev;
}

void
strela_dev_deinit(strela_dev *dev) {
	if (dev->initialized) {
		if (close(dev->fd) == -1) {
			perror(
				"On STRELA close(2) can fail only if it passed a bad file "
				"descriptor (EBADF). This is a non recoverable programmer"
				" error because strela_dev_deinit should never fail."
			);
			abort();
		}

		if (munmap(dev->base, STRELA_DATA_REGION_SIZE) == -1) {
			perror(
				"munmap(3) can only fail because it was passed bad "
				"arguments (EINVAL). This is non recoverable programmer "
				"error because strela_dev_deinit should never fail.");
			abort();
		}

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

strela_kernel
strela_kernel_get(strela_dev *dev) {
	strela_kernel res = {0};
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
strela_kernel_put(strela_dev *dev, strela_kernel kernel) {
	if (strela_dev_ok(dev)) {
		if (!kernel.valid || kernel.handle >= 128) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}

		if (strela_dev_ok(dev)) {
			unsigned char *ptr = dev->kernel_pool.buf + kernel.handle * STRELA_KERNEL_SIZE * 4;
			pool_free(&dev->kernel_pool, ptr);
		}
	}
}

void
strela_kernel_put_all(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		pool_free_all(&dev->kernel_pool);
	}
}

strela_buffer
strela_buffer_alloc(strela_dev *dev, size_t size_words) {
	strela_buffer res = {0};
	if (strela_dev_ok(dev)) {
		size_t size_bytes = size_words * sizeof (strela_word);
		if (size_bytes >= size_words) {
			unsigned char *ptr = arena_alloc_align(&dev->buffer_arena, size_bytes, sizeof (strela_word));
			if (ptr) {
				res.valid = true;
				res.size_words = size_words;
				res.offset_words_from_base = ((uintptr_t) ptr - (uintptr_t) dev->base)
					/ (uintptr_t) sizeof (strela_word);
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
strela_buffer_ptr(strela_dev *dev, strela_buffer buffer) {
	// We do not want to return null pointers and requiring that this function
	// is used only when no error has ever happened is a reasonable request for
	// the programmer.
	if (!buffer.valid) abort();
	return (strela_word *) (
		(uintptr_t) dev->base
		+ (uintptr_t) (buffer.offset_words_from_base*sizeof (strela_word))
	);
}

void
strela_buffer_free(strela_dev *dev, strela_buffer buffer) {
	if (strela_dev_ok(dev)) {
		if (!buffer.valid) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}
		// Bump allocator does nothing on free.
	}
}

void
strela_buffer_free_all(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		arena_free_all(&dev->buffer_arena);
	}
}

void
strela_config(strela_dev *dev, strela_kernel kernel, strela_conf *conf) {

	if (strela_dev_ok(dev)) {

		if (!kernel.valid || kernel.handle >= 128) {
			dev->res.errnum = -STRELA_ERR_BAD_ARG;
		}

		struct strela_ctrl ctrl = {
			.conf_offset = kernel.handle * STRELA_KERNEL_SIZE * 4,
			.conf_count = STRELA_KERNEL_SIZE,

			.inp0_offset = conf->inp0_offset,
			.inp0_count = conf->inp0_count,
			.inp0_stride = conf->inp0_stride,

			.inp1_offset = conf->inp1_offset,
			.inp1_count = conf->inp1_count,
			.inp1_stride = conf->inp1_stride,

			.inp2_offset = conf->inp2_offset,
			.inp2_count = conf->inp2_count,
			.inp2_stride = conf->inp2_stride,

			.inp3_offset = conf->inp3_offset,
			.inp3_count = conf->inp3_count,
			.inp3_stride = conf->inp3_stride,

			.out0_offset = conf->out0_offset,
			.out0_count = conf->out0_count,

			.out1_offset = conf->out1_offset,
			.out1_count = conf->out1_count,

			.out2_offset = conf->out2_offset,
			.out2_count = conf->out2_count,

			.out3_offset = conf->out3_offset,
			.out3_count = conf->out3_count,
		};

		if (strela_dev_ok(dev)
			&& ioctl(dev->fd, IOCTL_STRELA_CONTROL, &ctrl) == -1) {
			perror("IOCTL_STRELA_CONTROL");
			dev->res.errnum = errno;
		}

		if (strela_dev_ok(dev)
			&& ioctl(dev->fd, IOCTL_STRELA_CONFIG) == -1) {
			perror("IOCTL_STRELA_CONFIG");
			dev->res.errnum = errno;
		}
	}
}

void
strela_execute(strela_dev *dev) {
	if (strela_dev_ok(dev)) {
		if (ioctl(dev->fd, IOCTL_STRELA_EXEC) == -1) {
			perror("IOCTL_STRELA_EXEC");
			dev->res.errnum = errno;
		}
	}
}
