#ifndef STRELA_H
#define STRELA_H

/* TODO: functions that abort if called when the library has an error should
 * have a special suffix like strela_buffer_to_ptrA
 * TODO: rename strela_conf to strela_dma_conf
 */

/* STRELA library.
 * This library uses "monadic" error handling.
 * https://youtu.be/QpAhX-gsHMs?si=UzgvcwxAFiDjpbkB&t=1391
 */

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
	STRELA_KERNEL_SIZE = STRELA_NPE * 5,
};

typedef int32_t strela_word;

enum strela_err {
	STRELA_ERR_OK,
	STRELA_ERR_BAD_ARG,
	STRELA_ERR_NO_MEM,
};
typedef enum strela_err strela_err;

/* STRELA result type.
 * Negative numbers are used for strela_err while positive ones shall be
 * interpreted as classic errno(3) errors.
 */
typedef struct strela_res strela_res;
struct strela_res { int errnum; };

/* STRELA per device context.
 */
typedef struct strela_dev strela_dev;

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
	size_t offset_words_from_base;
	size_t size_words;
};

/* STRELA I/O configuration.
 * This struct closely mirrors the data used for ioctl but it is used to
 * decouple this generic header from the OS specific one. Everything is in
 * STRELA words.
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
 * count could be returned. If an error occurs errno is set. */
int         strela_device_count(unsigned *count);

strela_dev *strela_dev_init(unsigned which_strela);
void        strela_dev_deinit(strela_dev *dev);
bool        strela_dev_ok(strela_dev *dev);
void        strela_dev_reset_err(strela_dev *dev);
strela_res  strela_dev_get_err(strela_dev *dev);
bool        strela_dev_initialized(strela_dev *dev);

/* STRELA kernels management.
 */
strela_kernel strela_kernel_alloc(strela_dev *dev);
void          strela_kernel_set(strela_dev *dev, strela_kernel kernel,
                                const uint32_t data[STRELA_KERNEL_SIZE]);
void          strela_kernel_free(strela_dev *dev, strela_kernel kernel);
void          strela_kernel_free_all(strela_dev *dev);

/* STRELA input and output data buffers management.
 * A pointer can be obtained to read and write data.
 * TODO: right now it is implemented as a bump allocator but in the future a
 * free list implementation should be used.
 */
strela_buffer strela_buffer_alloc(strela_dev *dev, size_t size);
strela_word  *strela_buffer_to_ptr(strela_dev *dev, strela_buffer buffer);
strela_buffer strela_buffer_from_ptr(strela_dev *dev, const void *ptr);
void          strela_buffer_set_data(strela_dev *dev, strela_buffer buffer,
                                     const strela_word *ptr);
void          strela_buffer_get_data(strela_dev *dev, strela_buffer buffer,
                                     strela_word *ptr);
void          strela_buffer_free(strela_dev *dev, strela_buffer buffer);
void          strela_buffer_free_all(strela_dev *dev);
// void          strela_buffer_flush(strela_dev *dev, strela_buffer buffer,
//                                   size_t offset, size_t count);
// void          strela_buffer_inval(strela_dev *dev, strela_buffer buffer,
//                                   size_t offset, size_t count);

/* The config function configures both the kernel and the I/O configuration.
 * In the future this functionality should be split to allow reuse of the same
 * kernel with different inputs.
 */
void strela_config(strela_dev *dev, strela_kernel kernel, strela_conf *conf);
void strela_execute(strela_dev *dev);

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
