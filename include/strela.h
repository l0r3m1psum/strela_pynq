/* High level design rationale of the STRELA user space library.
 *
 * This library is meant to be used by programmers and the IREE HAL.
 * This library uses "monadic" error handling for user convenience.
 * https://youtu.be/QpAhX-gsHMs?si=UzgvcwxAFiDjpbkB&t=1391
 *
 * Unrecoverable programmer error will cause abort(3) to be called. This library
 * very much expects to be used by a single thread.
 *
 * TODO: functions that abort if called when the library has an error should
 * have a special suffix like strela_buffer_to_ptrA?
 */

#ifndef STRELA_H
#define STRELA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* STRELA hardware configuration constants.
 */
enum {
	STRELA_PE_ROWS = 4,
	STRELA_PE_COLS = 4,
	STRELA_NPE = STRELA_PE_ROWS * STRELA_PE_COLS,
	/* NOTE(Diego): in the future this will become MAX_SIZE because kernels will
	 * will have variable length. */
	STRELA_KERNEL_SIZE = STRELA_NPE * 5,
};

#define STRELA_DRM_NAME "strela"

typedef int32_t strela_word;

enum strela_err {
	STRELA_ERR_OK,
	STRELA_ERR_BAD_ARG,
	STRELA_ERR_NO_MEM,
};
typedef enum strela_err strela_err;

/* Negative numbers are used for strela_err while positive ones shall be
 * interpreted as classic errno(3) errors.
 */
typedef struct strela_res strela_res;
struct strela_res { int errnum; };

/* STRELA per device context. Errors are contained in this struct to allow
 * monadic error handling. If the device fails initialization it needs to be
 * reinitialized to be used if it has a normal error (e.g. allocating a buffer
 * too big for the available memory) that error can be reset to continue using
 * the correctly initialized device.
 */
typedef struct strela_dev strela_dev;

/* This function returns 0 on success and -1 if an error happens. An incomplete
 * count could be returned. If an error occurs errno is set. */
int         strela_dev_count(unsigned *count);
/* The pointer returned by this function is never null. This function can be
 * called multiple times on the same device and it just reinitializes its
 * resources i.e. allocators and jobs. */
strela_dev *strela_dev_init(unsigned which_strela);
void        strela_dev_deinit(strela_dev *dev);
bool        strela_dev_ok(strela_dev *dev);
void        strela_dev_reset_err(strela_dev *dev);
strela_res  strela_dev_get_err(strela_dev *dev);
/* Being initialized essentially means that the file descriptor is available,
 * together with the handles and address maps for both kernels and tensors...
 * This makes it possible for parts of the library to fail but still recover
 * e.g. tensor allocation can fail but if the device is not initialized there is
 * part of the address space mapped for it and we can't recover from that error
 * by freeing other tensors and then calling strela_dev_reset_err. */
bool        strela_dev_initialized(strela_dev *dev);

/* STRELA kernels bitstream management. There is no need to do memory
 * synchronization operations since it is all handled inside the functions.
 */

typedef struct strela_kernel strela_kernel;
struct strela_kernel {
	// TODO(Diego): the kernel should keep track it it has been set or not.
	bool valid;
	unsigned handle;
};

strela_kernel strela_kernel_alloc(strela_dev *dev);
void          strela_kernel_set(strela_dev *dev, strela_kernel kernel,
                                const uint32_t data[STRELA_KERNEL_SIZE]);
void          strela_kernel_free(strela_dev *dev, strela_kernel kernel);
void          strela_kernel_free_all(strela_dev *dev);

/* STRELA input and output data buffers management. This memory is handled with
 * in-band data structures for simplicity of implementation (as with the kernels
 * bistream memory) but in this case the user needs to be carefull to not
 * overwite neightbours data because it could corrupt the allocator metadata.
 */

typedef struct strela_buffer strela_buffer;
struct strela_buffer {
	bool valid;
	size_t offset_words_from_base;
	size_t size_words;
};

strela_buffer strela_buffer_alloc(strela_dev *dev, size_t size);
strela_word  *strela_buffer_to_ptr(strela_dev *dev, strela_buffer buffer);
strela_buffer strela_buffer_from_ptr(strela_dev *dev, const void *ptr);
void          strela_buffer_set_data(strela_dev *dev, strela_buffer buffer,
                                     const strela_word *ptr);
void          strela_buffer_get_data(strela_dev *dev, strela_buffer buffer,
                                     strela_word *ptr);
void          strela_buffer_free(strela_dev *dev, strela_buffer buffer);
void          strela_buffer_free_all(strela_dev *dev);
void          strela_buffer_flush(strela_dev *dev, strela_buffer buffer,
                                  size_t offset, size_t count);
/* This functino can fail if the data to invalidate is not cache aligned. This
 * is because a cache line is the minimal unit of invalidation. Invalidating
 * cache lines effectivelly means deleting their content from the cache and
 * reading it back from memory. We allocate the metadata in a separate cache
 * line outside the buffer memory and disallow memory syncs outise the buffer
 * memory this keeps things safe. */
void          strela_buffer_inval(strela_dev *dev, strela_buffer buffer,
                                  size_t offset, size_t count);

/* The config function configures both the kernel and the I/O configuration.
 * In the future this functionality should be split to allow reuse of the same
 * kernel with different inputs.
 */

/* STRELA I/O configuration.
 * This struct closely mirrors the data used for ioctl but it is used to
 * decouple this generic header from the OS specific one. Everything is in
 * STRELA words.
 * TODO(Diego): rename strela_conf to strela_dma_conf
 * TODO(Diego): use arays and proper data types for the bindings...
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

typedef struct strela_job strela_job;
struct strela_job {
	bool valid;
	unsigned handle;   /* index of a slot in the device */
};

typedef enum { STRELA_JOB_PENDING, STRELA_JOB_OK, STRELA_JOB_FAILED } strela_job_state;

strela_job       strela_job_config(strela_dev *dev, strela_kernel kernel, strela_conf *conf);
void             strela_job_free(strela_dev *dev, strela_job job);
void             strela_job_free_all(strela_dev *dev);
bool             strela_job_submit(strela_dev *dev, strela_job job);   /* false: queue full */
strela_job_state strela_job_poll(strela_dev *dev, strela_job job);
bool             strela_job_wait(strela_dev *dev, strela_job job, int timeout_ms);
int              strela_job_fd(strela_dev *dev, strela_job job);       /* pollable */

void strela_config(strela_dev *dev, strela_kernel kernel, strela_conf *conf);
strela_job_state strela_execute(strela_dev *dev);

#ifdef STRELA_TESTING_BITSTREAMS
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

static const uint32_t relu_kernel_bitstream[STRELA_KERNEL_SIZE] = {
    0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 12
    0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 8
    0x00004083, 0x20CC0300, 0x000000A0, 0x00000000, 0x00000000, // 4
    0x00000241, 0x020C0300, 0x00000099, 0x00000000, 0x00000000, // 0

    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 13
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 9
    0x00000011, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 5
    0x00400008, 0x00000200, 0x00000000, 0x00000000, 0x00000000, // 1

    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 14
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 10
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 6
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 2

    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 15
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 11
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 7
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 3
};
#endif

#ifdef __cplusplus
}
#endif

#endif
