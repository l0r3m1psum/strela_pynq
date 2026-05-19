#ifndef STRELA_H
#define STRELA_H

/* STRELA library.
 * This library uses signed sizes and "monadic" error handling.
 * https://youtu.be/QpAhX-gsHMs?si=UzgvcwxAFiDjpbkB&t=1391
 * https://c3-lang.org/blog/unsigned-sizes-a-five-year-mistake/
 * https://www.reddit.com/r/programming/comments/1t20rdz/comment/ojkeig9/
 *
 */

#include <stdint.h>
#include <stdbool.h>

/* STRELA hardware configuration constants.
 */
enum {
	STRELA_NPE = 16,
	STRELA_KERNEL_SIZE = STRELA_NPE * 5,
};

typedef uint32_t strela_word;

typedef enum strela_err strela_err;
enum strela_err {
	STRELA_ERR_OK,
	STRELA_ERR_ARG,
};

/* STRELA result type.
 * Negative numbers are used for strela_err while positive ones shall be
 * interpreted as classic errno(3) errors.
 */
typedef struct strela_res strela_res;
struct strela_res { int errnum; };

inline bool strela_res_ok(strela_res res) { return res.errnum == STRELA_ERR_OK; }

/* STRELA per device context.
 */
typedef struct strela_ctx strela_ctx;
struct strela_ctx {
	int fd;
	void *base;

	strela_res res;
	// TODO: pool allocator
	// TODO: bump allocator
};

/* STRELA kernel handle.
 */
typedef struct strela_kernel strela_kernel;
struct strela_kernel {
	int handle;
};

/* STRELA buffer handle.
 */
typedef struct strela_buffer strela_buffer;
struct strela_buffer {
	int offset; /* Can be used as an offset in strela_conf */
	int size;
};

/* STRELA I/O configuration.
 * This struct closely mirrors the data used for ioctl but it is used to
 * decouple this generic header from the OS specific one.
 */
typedef struct strela_conf strela_conf;
struct strela_conf {
	int inp0_offset, inp0_count, inp0_stride;
	int inp1_offset, inp1_count, inp1_stride;
	int inp2_offset, inp2_count, inp2_stride;
	int inp3_offset, inp3_count, inp3_stride;

	int out0_offset, out0_count;
	int out1_offset, out1_count;
	int out2_offset, out2_count;
	int out3_offset, out3_count;
};

strela_res strela_device_count(int *count);
strela_ctx strela_ctx_init(int which_strela);
void       strela_ctx_deinit(strela_ctx *ctx);

/* STRELA kernels management.
 * A offset can be obtained and used in strela_conf.
 */
strela_kernel strela_kernel_get(strela_ctx *ctx);
int           strela_kernel_offset(strela_ctx *ctx, strela_kernel kernel);
void          strela_kernel_set(strela_ctx *ctx, strela_kernel kernel,
                                uint32_t data[STRELA_KERNEL_SIZE]);
void          strela_kernel_put(strela_ctx *ctx, strela_kernel kernel);

/* STRELA input and output data buffers management.
 * A pointer can be obtained to read and write data.
 * TODO: right now it is implemented as a bump allocator but in the future a
 * free list implementation should be used.
 */
strela_buffer strela_buffer_alloc(strela_ctx *ctx, int size);
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
