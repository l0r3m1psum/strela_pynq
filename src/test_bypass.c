#include "cgra_dma.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <sys/mman.h>
#include <stdint.h>

#define BYPASS_KRNL_NPE (16)
#define BYPASS_KRNL_SIZE (BYPASS_KRNL_NPE * 5)

//#define BUFF_SIZE ((32 * 1024) / 8) // 32 KB
//#define BUFF_SIZE ((16 * 1024) / 8) // 16 KB
#define BUFF_SIZE (10) // 40 B

#define MAX_PRINTED_ERRORS 10

// 4x4
// 0 4 8  12
// 1 5 9  13
// 2 6 10 14
// 3 7 11 15
static const uint32_t bypass_kernel[BYPASS_KRNL_SIZE] = {
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
    0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000  // 3
};

static int
validate_buf(const uint32_t *out, const uint32_t *gold) {
    int errors = 0;

    for (int i = 0; i < BUFF_SIZE; i++) {
        int val = out[i];

        if (gold[i] != val) {
            errors++;
            if (errors <= MAX_PRINTED_ERRORS) {
                printf("%d : %d : %d\n", i, val, gold[i]);
            }
        }
    }

    return errors;
}

static void
init_buf(uint32_t *in, uint32_t *out, uint32_t *gold) {
    for(int i = 0; i < BUFF_SIZE; i++) {
        in[i] = i;
    }

    for(int i = 0; i < BUFF_SIZE; i++) {
        gold[i] = in[i];
    }

    for(int i = 0; i < BUFF_SIZE; i++) {
        out[i] = -1;
    }
}

int main(void) {

    int file_desc = open(DEVICE_PATH, O_RDWR);

    if (file_desc == -1) {
        perror("open");
        return 1;
    }

    uint32_t *mmap_ptr = mmap(NULL, CGRA_DATA_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, file_desc, 0);

    if (mmap_ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    enum {
        MEGABYTE      = 0x100000,
        CONFIG_OFFSET = 0*MEGABYTE / CGRA_WORD_SIZE,
        INPUT_OFFSET  = 1*MEGABYTE / CGRA_WORD_SIZE,
        OUTPUT_OFFSET = 2*MEGABYTE / CGRA_WORD_SIZE,
    };

    uint32_t *cgra_kernel = mmap_ptr + CONFIG_OFFSET;
    uint32_t *mem_inp = mmap_ptr + INPUT_OFFSET;
    uint32_t *mem_out = mmap_ptr + OUTPUT_OFFSET;
    uint32_t gold[BUFF_SIZE] = {0};

    init_buf(mem_inp, mem_out, gold);
    memcpy(cgra_kernel, bypass_kernel, sizeof bypass_kernel);

    printf("input: "); for (int i = 0; i < BUFF_SIZE; i++) printf("%d ", mem_inp[i]); puts("");
    printf("output: "); for (int i = 0; i < BUFF_SIZE; i++) printf("%d ", mem_out[i]); puts("");
    printf("gold: "); for (int i = 0; i < BUFF_SIZE; i++) printf("%d ", gold[i]); puts("");
    printf("kernel: "); for (int i = 0; i < BYPASS_KRNL_SIZE; i++) printf("0x%x ", cgra_kernel[i]); puts("");

    CGRA_control_t cgra_ctrl = {
        .conf_offs = CONFIG_OFFSET,
        .conf_count = BYPASS_KRNL_SIZE,

        .in0_offs = INPUT_OFFSET,
        .in0_count = BUFF_SIZE,
        .in0_stride = CGRA_WORD_SIZE,

        .out0_offs = OUTPUT_OFFSET,
        .out0_count = BUFF_SIZE,
    };

    if (ioctl(file_desc, IOCTL_CGRA_CONTROL, &cgra_ctrl) == -1) {
        perror("IOCTL_CGRA_CONTROL");
        return 1;
    }

    if (ioctl(file_desc, IOCTL_CGRA_CONFIG) == -1) {
        perror("IOCTL_CGRA_CONFIG");
        return 1;
    }

    if (ioctl(file_desc, IOCTL_CGRA_EXEC) == -1) {
        perror("IOCTL_CGRA_EXEC");
        return 1;
    }

    int errors = validate_buf(mem_out, gold);

    if (errors)
        printf("... FAIL\n");
    else
        printf("... PASS\n");

    return 0;
}
