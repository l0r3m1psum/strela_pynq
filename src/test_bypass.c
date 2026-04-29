#include "cgra_dma.h"

#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <sys/mman.h>
#include <stdint.h>

enum {
    MEGABYTE      = 0x100000,
    CONFIG_OFFSET = 0*MEGABYTE / CGRA_WORD_SIZE,
    INPUT_OFFSET  = 1*MEGABYTE / CGRA_WORD_SIZE,
    OUTPUT_OFFSET = 2*MEGABYTE / CGRA_WORD_SIZE,
};

enum { BUFF_SIZE = 10, }; // 40 B

typedef struct cgra_ctx cgra_ctx;
struct cgra_ctx {
    int fd;
    uint32_t *base;
};

static bool
cgra_ctx_init(cgra_ctx *ctx) {
    int fd = open(DEVICE_PATH, O_RDWR);

    if (fd == -1) {
        perror("open");
        return false;
    }

    uint32_t *base = mmap(NULL, CGRA_DATA_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return false;
    }

    ctx->fd = fd;
    ctx->base = base;
}

static int
cgra_bypass(
    struct cgra_ctx *ctx,
    const uint32_t *input,
    uint32_t *output,
    size_t len
) {
    enum {
        KRNL_NPE = 16,
        KRNL_SIZE = KRNL_NPE * 5,
    };

    static const uint32_t bypass_kernel[KRNL_SIZE] = {
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

    if (len > MEGABYTE/CGRA_WORD_SIZE) {
        fprintf(stderr, "input too big\n");
        return false;
    }

    uint32_t *mem_cfg = ctx->base + CONFIG_OFFSET;
    uint32_t *mem_inp = ctx->base + INPUT_OFFSET;
    const uint32_t *mem_out = ctx->base + OUTPUT_OFFSET;

    memcpy(mem_cfg, bypass_kernel, sizeof bypass_kernel);
    memcpy(mem_inp, input, sizeof *input * len);

    uint32_t len1 = len/4;
    uint32_t len2 = len1 + len%4;

    CGRA_control_t cgra_ctrl = {
        .conf_offs = CONFIG_OFFSET, .conf_count = KRNL_SIZE,

        .in0_offs = INPUT_OFFSET + len1*0, .in0_count = len1, .in0_stride = CGRA_WORD_SIZE,
        // .in1_offs = INPUT_OFFSET + len1*1, .in1_count = len1, .in1_stride = CGRA_WORD_SIZE,
        // .in2_offs = INPUT_OFFSET + len1*2, .in2_count = len1, .in2_stride = CGRA_WORD_SIZE,
        // .in3_offs = INPUT_OFFSET + len1*3, .in3_count = len2, .in3_stride = CGRA_WORD_SIZE,

        .out0_offs = OUTPUT_OFFSET + len1*0, .out0_count = len1,
        // .out1_offs = OUTPUT_OFFSET + len1*1, .out1_count = len1,
        // .out2_offs = OUTPUT_OFFSET + len1*2, .out2_count = len1,
        // .out3_offs = OUTPUT_OFFSET + len1*3, .out3_count = len2,
    };

    if (ioctl(ctx->fd, IOCTL_CGRA_CONTROL, &cgra_ctrl) == -1) {
        perror("IOCTL_CGRA_CONTROL");
        return false;
    }

    if (ioctl(ctx->fd, IOCTL_CGRA_CONFIG) == -1) {
        perror("IOCTL_CGRA_CONFIG");
        return false;
    }

    if (ioctl(ctx->fd, IOCTL_CGRA_EXEC) == -1) {
        perror("IOCTL_CGRA_EXEC");
        return false;
    }

    memcpy(output, mem_out, sizeof *output * len);

    return true;
}

static bool
cgra_relu(
    struct cgra_ctx *ctx,
    const uint32_t *input,
    uint32_t *output,
    size_t len
) {
    enum {
        KRNL_NPE = 16, KRNL_NROUTERS = 4,
        KRNL_SIZE = KRNL_NPE * 5 + KRNL_NROUTERS,
    };

    static const uint32_t relu_kernel[KRNL_SIZE] = {
        0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 12
        0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 8
        0x00004083, 0x00CC0300, 0x000000A0, 0x00000000, 0x00000000, // 4
        0x00000241, 0x020C0300, 0x00000098, 0x00000000, 0x00000000, // 0

        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 13
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 9
        0x00000011, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 5
        0x00400008, 0x00000200, 0x00000000, 0x00000000, 0x00000000, // 1

        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 14
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 10
        0x00000041, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 6
        0x00000802, 0x00000100, 0x00000000, 0x00000000, 0x00000000, // 2

        0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 15
        0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 11
        0x04000089, 0x21CC0300, 0x000000A0, 0x00000000, 0x00000000, // 7
        0x00000211, 0x020C0300, 0x00000098, 0x00000000, 0x00000000, // 3
    };

    if (len > MEGABYTE/CGRA_WORD_SIZE) {
        return false;
    }

    uint32_t *mem_cfg = ctx->base + CONFIG_OFFSET;
    uint32_t *mem_inp = ctx->base + INPUT_OFFSET;
    const uint32_t *mem_out = ctx->base + OUTPUT_OFFSET;

    memcpy(mem_cfg, relu_kernel, sizeof relu_kernel);
    memcpy(mem_inp, input, sizeof *input * len);

    uint32_t len1 = len/2;
    uint32_t len2 = len1 + len%2;

    CGRA_control_t cgra_ctrl = {
        .conf_offs = CONFIG_OFFSET,
        .conf_count = KRNL_SIZE,

        .in0_offs = INPUT_OFFSET + len1*0, .in0_count = len1, .in0_stride = CGRA_WORD_SIZE,
        // .in3_offs = INPUT_OFFSET + len1*1, .in3_count = len2, .in3_stride = CGRA_WORD_SIZE,

        .out0_offs = OUTPUT_OFFSET + len1*0, .out0_count = len1,
        // .out3_offs = OUTPUT_OFFSET + len1*1, .out3_count = len2,
    };

    if (ioctl(ctx->fd, IOCTL_CGRA_CONTROL, &cgra_ctrl) == -1) {
        perror("IOCTL_CGRA_CONTROL");
        return false;
    }

    if (ioctl(ctx->fd, IOCTL_CGRA_CONFIG) == -1) {
        perror("IOCTL_CGRA_CONFIG");
        return false;
    }

    if (ioctl(ctx->fd, IOCTL_CGRA_EXEC) == -1) {
        perror("IOCTL_CGRA_EXEC");
        return false;
    }

    memcpy(output, mem_out, sizeof *output * len);

    return true;
}

static int max(int a, int b) { return a > b ? a : b; }

static void inspect_mem(const char *name, int32_t *mem, size_t len) {
    printf("%s: ", name);
    for (size_t i = 0; i < len; i++) printf("%d ", mem[i]);
    puts("");
}

#define countof(x) (sizeof (x) / sizeof *(x))

int main(void) {

    cgra_ctx ctx = {0};
    if (cgra_ctx_init(&ctx) == -1) {
        fprintf(stderr, "Cannot initialize CGRA context\n");
        return 1;
    }

    int errors = 0;

    {
        static int32_t input[BUFF_SIZE];
        static int32_t output[BUFF_SIZE];
        static int32_t output_ref[BUFF_SIZE];

        for (int i = 0; i < BUFF_SIZE; i++) {
            input[i] = i;
            output_ref[i] = input[i];
        }

        if (!cgra_bypass(&ctx, input, output, BUFF_SIZE)) {
            fprintf(stderr, "Can't execute bypass kernel\n");
            return 1;
        }

        if (memcmp(output, output_ref, sizeof output) != 0) {
            fprintf(stderr, "bypass kernel made mistakes\n");
            errors++;
        } else {
            printf("Bypass pass\n");
        }

    }

    {
        static int32_t input[BUFF_SIZE];
        static int32_t output[BUFF_SIZE];
        static int32_t output_ref[BUFF_SIZE];

        for (int i = 0; i < BUFF_SIZE; i++) {
            input[i] = i%2 == 0 ? 1 : -1;
            output_ref[i] = max(0, input[i]);
        }

        if (!cgra_relu(&ctx, input, output, BUFF_SIZE)) {
            fprintf(stderr, "Can't execute relu kernel\n");
            return 1;
        }

        if (memcmp(output, output_ref, sizeof output) != 0) {
            fprintf(stderr, "relu kernel made mistakes\n");
            errors++;
        } else {
            printf("ReLU pass\n");
        }

    }

    return errors != 0;
}
