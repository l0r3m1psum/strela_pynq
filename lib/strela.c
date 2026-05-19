#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "strela.h"
#include "strela_ioctl.h"

#define COUNTOF(x) (sizeof (x) / sizeof *(x))

strela_res
strela_device_count(int *count) {
	int local_count = 0;
	char path_buf[] = "/dev/strelaX";
	int last_char = COUNTOF(path_buf) - 1 - 1;
	strela_res res = {0};

	// We need to rest errno to determine if access failed or the file just does
	// not exists.
	errno = 0;

	for (int i = 0; i < 9; i++) {
		path_buf[last_char] = '0' + i;
		if (access(path_buf, F_OK) == -1) {
			break;
		} else {
			local_count++;
		}
	}

	*count = local_count;
	res.errnum = errno;

	if (errno != 0) {
		perror("access");
		errno = 0;
	}

	return res;
}

strela_ctx
strela_ctx_init(int which_strela) {
	char path_buf[128] = {0};
	int ret = 0, fd = 0;
	uint32_t *base = NULL;
	strela_ctx ctx = {0};

	if (which_strela < 0) {
		ctx.res.errnum = -STRELA_ERR_ARG;
	}

	ret = snprintf(path_buf, sizeof path_buf, "/dev/strela%d", which_strela);

	assert(ret >= 0);

	if (strela_res_ok(ctx.res)) {
		fd = open(path_buf, O_RDWR);
		if (fd == -1) {
			perror("open");
			ctx.res.errnum = errno;
		}
	}

	if (strela_res_ok(ctx.res)) {
		base = mmap(NULL, STRELA_DATA_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (base == MAP_FAILED) {
			perror("mmap");
			ctx.res.errnum = errno;
		}
	}

	if (!strela_res_ok(ctx.res)) {
		close(fd);
	} else {
		ctx.fd = fd;
		ctx.base = base;
	}

	// TODO: init kernel memory pool (say 128)
	// TODO: init data bump allocator

	return ctx;
}

void
strela_ctx_deinit(strela_ctx *ctx) {
	if (strela_res_ok(ctx->res)) {
		if (close(ctx->fd) == -1) {
			perror("close");
			ctx->res.errnum = errno;
		}

		if (strela_res_ok(ctx->res)) {
			if (munmap(ctx->base, STRELA_DATA_REGION_SIZE) == -1) {
				perror("munmap");
				ctx->res.errnum = errno;
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
	// Can allocate only multiples of STRELA_WORD_SIZE aligned to STRELA_WORD_SIZE
	strela_buffer res = {0};
	return res;
}

strela_word *
strela_buffer_ptr(strela_ctx *ctx, strela_buffer) {
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

// TODO: dato un kernel, e dei dati devo generare il controllo.
// la configurazione dipende dal kernel, quindi deve essere fornita dall'utente
// io devo solo fornire la funzione per scrivere il controllo, che deve flushare
// la memoria del kernel usato e dei dati in input...
