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

enum {
	STRELA_MAX_DEV = 8,
};

// TODO: strela_ctx -> strela_dev
// TODO: add initialized flag that can be resetted only by closing the device
// hence all operation on it are noops including error reset. This flag should
// also handle multiple calls of init for the same device by just returning the
// pointer without doing the initialization (idempotency).
// TODO: how can open and mmap fail?

struct strela_ctx {
	int fd;
	void *base;

	strela_res res;
	Pool kernel_pool;
	Arena buffer_arena;
};

static strela_ctx contexes[STRELA_MAX_DEV];

static bool strela_res_ok(strela_res res) { return res.errnum == STRELA_ERR_OK; }

int
strela_device_count(unsigned *count) {
	int local_count = 0;
	char path_buf[] = "/dev/strelaX";
	int last_char = COUNTOF(path_buf) - 1 - 1;
	int res = 0;

	// TODO: use snprintf
	static_assert(STRELA_MAX_DEV < 10, "maximum one decimal digit.");
	// We need to rest errno to determine if access failed or the file just does
	// not exists.
	// TODO: restore errno original value...
	errno = 0;

	for (int i = 0; i < STRELA_MAX_DEV; i++) {
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
	}

	return res;
}

strela_ctx *
strela_ctx_init(unsigned which_strela) {
	char path_buf[128] = {0};
	int ret = 0, fd = -1;
	void *base = NULL;
	strela_ctx *ctx = NULL;

	if (which_strela >= STRELA_MAX_DEV) {
		// We consider this an unrecoverable programmer error. Otherwise it
		// becomes cumbersome to handle this corner case.
		abort();
	}

	ctx = &contexes[which_strela];

	ret = snprintf(path_buf, sizeof path_buf, "/dev/strela%d", which_strela);
	assert(ret >= 0);

	if (strela_res_ok(ctx->res)) {
		fd = open(path_buf, O_RDWR);
		if (fd == -1) {
			perror("open");
			ctx->res.errnum = errno;
		}
	}

	if (strela_res_ok(ctx->res)) {
		base = mmap(NULL, STRELA_DATA_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (base == MAP_FAILED) {
			perror("mmap");
			ctx->res.errnum = errno;
		}
	}

	if (!strela_res_ok(ctx->res)) {
		if (fd != -1) close(fd);
	} else {
		ctx->fd = fd;
		ctx->base = base;
		pool_init(
			&ctx->kernel_pool,
			base,
			4*STRELA_KERNEL_SIZE*128,
			4*STRELA_KERNEL_SIZE,
			1 // Kernels can be byte aligned.
		);
		arena_init(
			&ctx->buffer_arena,
			(unsigned char *) base + 4*STRELA_KERNEL_SIZE*128,
			STRELA_DATA_REGION_SIZE - 4*STRELA_KERNEL_SIZE*128
		);
	}

	return ctx;
}

void
strela_ctx_deinit(strela_ctx *ctx) {
	if (strela_res_ok(ctx->res)) {
		if (close(ctx->fd) == -1) {
			perror(
				"On STRELA close(2) can fail only if it passed a bad file "
				"descriptor (EBADF). This is a non recoverable programmer"
				" error because strela_ctx_deinit should never fail."
			);
			abort();
		}

		if (munmap(ctx->base, STRELA_DATA_REGION_SIZE) == -1) {
			perror(
				"munmap(3) can only fail because it was passed bad "
				"arguments (EINVAL). This is non recoverable programmer "
				"error because strela_ctx_deinit should never fail.");
			abort();
		}
	}
}

bool
strela_ctx_ok(strela_ctx *ctx) {
	return strela_res_ok(ctx->res);
}

void
strela_ctx_reset_err(strela_ctx *ctx) {
	ctx->res.errnum = 0;
}

strela_res
strela_ctx_get_err(strela_ctx *ctx) {
	return ctx->res;
}

// Pool allocator for kernels.//////////////////////////////////////////////////

strela_kernel
strela_kernel_get(strela_ctx *ctx) {
	strela_kernel res = {0};
	if (strela_ctx_ok(ctx)) {
		unsigned char *ptr = pool_alloc(&ctx->kernel_pool);
		if (ptr) {
			unsigned handle = ((uintptr_t) ptr - (uintptr_t) ctx->kernel_pool.buf)
				/ ((uintptr_t) STRELA_KERNEL_SIZE * (uintptr_t) 4);
			assert(handle < 128);
			res.valid = true;
			res.handle = handle;
		}
	}
	return res;
}

// TODO: do an out of band data structure for the pool allocator to be able to
// check if a handle is allocated or not and return STRELA_ERR_BAD_ARG if it is.
// struct { bool allocated; uint8_t next_free; } free_list[128];

void
strela_kernel_set(strela_ctx *ctx, strela_kernel kernel, const uint32_t data[STRELA_KERNEL_SIZE]) {
	if (strela_ctx_ok(ctx)) {
		if (!kernel.valid || kernel.handle >= 128) {
			ctx->res.errnum = -STRELA_ERR_BAD_ARG;
		}

		if (strela_ctx_ok(ctx)) {
			unsigned char *ptr = ctx->kernel_pool.buf + kernel.handle * STRELA_KERNEL_SIZE * 4;
			memcpy(ptr, data, STRELA_KERNEL_SIZE * 4);
		}
	}
}

void
strela_kernel_put(strela_ctx *ctx, strela_kernel kernel) {
	if (strela_ctx_ok(ctx)) {
		if (!kernel.valid || kernel.handle >= 128) {
			ctx->res.errnum = -STRELA_ERR_BAD_ARG;
		}

		if (strela_ctx_ok(ctx)) {
			unsigned char *ptr = ctx->kernel_pool.buf + kernel.handle * STRELA_KERNEL_SIZE * 4;
			pool_free(&ctx->kernel_pool, ptr);
		}
	}
}

void
strela_kernel_put_all(strela_ctx *ctx) {
	if (strela_ctx_ok(ctx)) {
		pool_free_all(&ctx->kernel_pool);
	}
}

// Bump allocator for data /////////////////////////////////////////////////////

strela_buffer
strela_buffer_alloc(strela_ctx *ctx, size_t size_words) {
	strela_buffer res = {0};
	size_t size_bytes = size_words * sizeof (strela_word); // TODO: check for overflow.
	unsigned char *ptr = arena_alloc_align(&ctx->buffer_arena, size_bytes, sizeof (strela_word));
	if (ptr) {
		res.valid = true;
		res.size_words = size_words;
		res.offset_words_from_base = ((uintptr_t) ptr - (uintptr_t) ctx->base)
			/ (uintptr_t) sizeof (strela_word);
	}
	return res;
}

strela_word *
strela_buffer_ptr(strela_ctx *ctx, strela_buffer buffer) {
	if (!buffer.valid) abort();
	return (strela_word *) (
		(uintptr_t) ctx->base
		+ (uintptr_t) (buffer.offset_words_from_base*sizeof (strela_word))
	);
}

void
strela_buffer_free(strela_ctx *ctx, strela_buffer buffer) {
	if (strela_ctx_ok(ctx)) {
		if (!buffer.valid) abort();
		// Bump allocator does nothing on free.
	}
}

void
strela_buffer_free_all(strela_ctx *ctx) {
	if (strela_ctx_ok(ctx)) {
		arena_free_all(&ctx->buffer_arena);
	}
}

void
strela_config(strela_ctx *ctx, strela_kernel kernel, strela_conf *conf) {

	if (strela_res_ok(ctx->res)) {

		if (!kernel.valid || kernel.handle >= 128) {
			ctx->res.errnum = -STRELA_ERR_BAD_ARG;
		}

		struct strela_ctrl ctrl = {
			.conf_offset = kernel.handle * STRELA_KERNEL_SIZE,
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

		if (strela_res_ok(ctx->res)
			&& ioctl(ctx->fd, IOCTL_STRELA_CONTROL, &ctrl) == -1) {
			perror("IOCTL_STRELA_CONTROL");
			ctx->res.errnum = errno;
		}

		if (strela_res_ok(ctx->res)
			&& ioctl(ctx->fd, IOCTL_STRELA_CONFIG) == -1) {
			perror("IOCTL_STRELA_CONFIG");
			ctx->res.errnum = errno;
		}
	}
}

void
strela_execute(strela_ctx *ctx) {
	if (strela_res_ok(ctx->res)) {
		if (ioctl(ctx->fd, IOCTL_STRELA_EXEC) == -1) {
			perror("IOCTL_STRELA_EXEC");
			ctx->res.errnum = errno;
		}
	}
}
