#ifndef STRELA_H
#define STRELA_H

/* STRELA library.
 * This library uses "monadic" error handling.
 * https://youtu.be/QpAhX-gsHMs?si=UzgvcwxAFiDjpbkB&t=1391
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* STRELA hardware configuration constants.
 */
enum {
	STRELA_NPE = 16,
	STRELA_KERNEL_SIZE = STRELA_NPE * 5,
};

typedef int32_t strela_word;

typedef enum strela_err strela_err;
enum strela_err {
	STRELA_ERR_OK,
	STRELA_ERR_NO_DEV,
};

/* STRELA result type.
 * Negative numbers are used for strela_err while positive ones shall be
 * interpreted as classic errno(3) errors.
 */
typedef struct strela_res strela_res;
struct strela_res { int errnum; };

/* STRELA per device context.
 */
typedef struct strela_ctx strela_ctx;

/* STRELA kernel handle.
 */
typedef struct strela_kernel strela_kernel;
struct strela_kernel {
	bool valid;
	unsigned handle;
};

/* STRELA buffer handle.
 */
typedef struct strela_buffer strela_buffer;
struct strela_buffer {
	bool valid;
	size_t offset; /* Can be used as an offset in strela_conf */
	size_t size;
};

/* STRELA I/O configuration.
 * This struct closely mirrors the data used for ioctl but it is used to
 * decouple this generic header from the OS specific one.
 */
typedef struct strela_conf strela_conf;
struct strela_conf {
	size_t inp0_offset, inp0_count, inp0_stride;
	size_t inp1_offset, inp1_count, inp1_stride;
	size_t inp2_offset, inp2_count, inp2_stride;
	size_t inp3_offset, inp3_count, inp3_stride;

	size_t out0_offset, out0_count;
	size_t out1_offset, out1_count;
	size_t out2_offset, out2_count;
	size_t out3_offset, out3_count;
};

/* This function returns 0 on success and -1 if an error happens. An incomplete
 * count could be returned. If an error occurs errno is set.  */
int         strela_device_count(unsigned *count);

strela_ctx *strela_ctx_init(unsigned which_strela);
void        strela_ctx_deinit(strela_ctx *ctx);
bool        strela_ctx_ok(strela_ctx *ctx);
void        strela_ctx_reset_err(strela_ctx *ctx);
strela_res  strela_ctx_get_err(strela_ctx *ctx);

/* STRELA kernels management.
 */
strela_kernel strela_kernel_get(strela_ctx *ctx);
void          strela_kernel_set(strela_ctx *ctx, strela_kernel kernel,
                                const uint32_t data[STRELA_KERNEL_SIZE]);
void          strela_kernel_put(strela_ctx *ctx, strela_kernel kernel);
void          strela_kernel_put_all(strela_ctx *ctx);

/* STRELA input and output data buffers management.
 * A pointer can be obtained to read and write data.
 * TODO: right now it is implemented as a bump allocator but in the future a
 * free list implementation should be used.
 */
strela_buffer strela_buffer_alloc(strela_ctx *ctx, size_t size);
strela_word  *strela_buffer_ptr(strela_ctx *ctx, strela_buffer buffer);
void          strela_buffer_free(strela_ctx *ctx, strela_buffer buffer);
void          strela_buffer_free_all(strela_ctx *ctx);

/* The config function configures both the kernel and the I/O configuration.
 * In the future this functionality should be split to allow reuse of the same
 * kernel with different inputs.
 */
void strela_config(strela_ctx *ctx, strela_kernel kernel, strela_conf *conf);
void strela_execute(strela_ctx *ctx);

#endif
