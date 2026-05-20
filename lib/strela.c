#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "strela.h"
#include "strela_ioctl.h"

#define COUNTOF(x) (sizeof (x) / sizeof *(x))

enum {
	STRELA_MAX_DEV = 8,
};

typedef struct strela_kernel_pool strela_kernel_pool;

struct strela_ctx {
	int fd;
	void *base;

	strela_res res;
	strela_kernel_pool *kernel_pool;
	// TODO: bump allocator
};

static strela_ctx contexes[STRELA_MAX_DEV];
static strela_ctx no_dev_context = { .res.errnum = STRELA_ERR_NO_DEV, };

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
	int ret = 0, fd = 0;
	uint32_t *base = NULL;
	strela_ctx *ctx = NULL;

	if (which_strela >= STRELA_MAX_DEV) {
		ctx = no_dev_context;
	}

	if (strela_res_ok(ctx->res)) {
		ctx = contexes[which_strela];
	}

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
		close(fd);
	} else {
		ctx->fd = fd;
		ctx->base = base;
	}

	// TODO: init kernel memory pool (say 128)
	// TODO: init data bump allocator

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

		if (strela_res_ok(ctx->res)) {
			if (munmap(ctx->base, STRELA_DATA_REGION_SIZE) == -1) {
				perror(
					"munmap(3) can only fail because it was passed bad "
					"arguments (EINVAL). This is non recoverable programmer "
					"error because strela_ctx_deinit should never fail.");
				abort()
			}
		}
	}
}

// Pool allocator for kernels.//////////////////////////////////////////////////

strela_kernel
strela_kernel_get(strela_ctx *ctx) {
	strela_kernel res = {0};
	return res;
}

void
strela_kernel_set(strela_ctx *ctx, strela_kernel kernel, uint32_t data[STRELA_KERNEL_SIZE]) {
	// memcpy
};

void
strela_kernel_put(strela_ctx *ctx, strela_kernel kernel) {
	;
}

// Bump allocator for data /////////////////////////////////////////////////////

strela_buffer
strela_buffer_alloc(strela_ctx *ctx, int size) {
	// Can allocate only multiples of sizeof (strela_word) aligned to sizeof (strela_word)
	strela_buffer res = {0};
	return res;
}

strela_word *
strela_buffer_ptr(strela_ctx *ctx, strela_buffer buffer) {
	if (!buffer.valid) abort();
	return NULL;
}

void
strela_buffer_free(strela_ctx *ctx, strela_buffer buffer) {
	// Bumb allocator free does nothing.
}

void
strela_buffer_free_all(strela_ctx *ctx) {
	// Set base pointer to 0
}

void
strela_config(strela_ctx *ctx, strela_kernel kernel, strela_conf *conf) {

	if (strela_res_ok(ctx->res)) {

		assert(false && "check that the kernel handle has been allocated and is in the valid range.");

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
