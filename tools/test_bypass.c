#include "strela_ioctl.h"

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
    CONFIG_OFFSET = 0*MEGABYTE / STRELA_WORD_SIZE,
    INPUT_OFFSET  = 1*MEGABYTE / STRELA_WORD_SIZE,
    OUTPUT_OFFSET = 2*MEGABYTE / STRELA_WORD_SIZE,
};

enum { BUFF_SIZE = 10, }; // 40 B

typedef struct strela_ctx strela_ctx;
struct strela_ctx {
    int fd;
    uint32_t *base;
};

static bool
strela_ctx_init(strela_ctx *ctx, const char *dev_path) {
    int fd = open(dev_path, O_RDWR);

    if (fd == -1) {
        perror("open");
        return false;
    }

    uint32_t *base = mmap(NULL, STRELA_DATA_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return false;
    }

    ctx->fd = fd;
    ctx->base = base;
}

// TODO: add an argument to decide where to route the data for testing.
// This makes it easy to see which columns are working.
static int
strela_bypass(
    struct strela_ctx *ctx,
    const uint32_t *input,
    uint32_t *output,
    size_t len
) {
    enum {
        KRNL_NPE = 16, KRNL_SIZE = KRNL_NPE * 5,
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

    if (len > MEGABYTE/STRELA_WORD_SIZE) {
        fprintf(stderr, "input too big\n");
        return false;
    }

    uint32_t *mem_cfg = ctx->base + CONFIG_OFFSET;
    uint32_t *mem_inp = ctx->base + INPUT_OFFSET;
    const uint32_t *mem_out = ctx->base + OUTPUT_OFFSET;

    memset(ctx->base, 0, STRELA_DATA_REGION_SIZE);

    memcpy(mem_cfg, bypass_kernel, sizeof bypass_kernel);
    memcpy(mem_inp, input, sizeof *input * len);

    uint32_t len1 = len/4;
    uint32_t len2 = len1 + len%4;

    struct strela_ctrl ctrl = {
        .conf_offset = CONFIG_OFFSET, .conf_count = KRNL_SIZE,

        .inp0_offset = INPUT_OFFSET + len1*0, .inp0_count = len1, .inp0_stride = STRELA_WORD_SIZE,
        .inp1_offset = INPUT_OFFSET + len1*1, .inp1_count = len1, .inp1_stride = STRELA_WORD_SIZE,
        .inp2_offset = INPUT_OFFSET + len1*2, .inp2_count = len1, .inp2_stride = STRELA_WORD_SIZE,
        .inp3_offset = INPUT_OFFSET + len1*3, .inp3_count = len2, .inp3_stride = STRELA_WORD_SIZE,

        .out0_offset = OUTPUT_OFFSET + len1*0, .out0_count = len1,
        .out1_offset = OUTPUT_OFFSET + len1*1, .out1_count = len1,
        .out2_offset = OUTPUT_OFFSET + len1*2, .out2_count = len1,
        .out3_offset = OUTPUT_OFFSET + len1*3, .out3_count = len2,
    };

    if (ioctl(ctx->fd, IOCTL_STRELA_CONTROL, &ctrl) == -1) {
        perror("IOCTL_STRELA_CONTROL");
        return false;
    }

    if (ioctl(ctx->fd, IOCTL_STRELA_CONFIG) == -1) {
        perror("IOCTL_STRELA_CONFIG");
        return false;
    }

    if (ioctl(ctx->fd, IOCTL_STRELA_EXEC) == -1) {
        perror("IOCTL_STRELA_EXEC");
        return false;
    }

    memcpy(output, mem_out, sizeof *output * len);

    return true;
}

static bool
strela_relu(
    struct strela_ctx *ctx,
    const uint32_t *input,
    uint32_t *output,
    size_t len
) {
    enum {
        KRNL_NPE = 16, KRNL_SIZE = KRNL_NPE * 5,
    };

    static const uint32_t relu_kernel[KRNL_SIZE] = {
        0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 12
        0x00000021, 0x00000000, 0x00000000, 0x00000000, 0x00000000, // 8
        0x00004083, 0x20CC0300, 0x000000A0, 0x00000000, 0x00000000, // 4
        0x00000241, 0x020C0300, 0x0000009A, 0x00000000, 0x00000000, // 0

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

    if (len > MEGABYTE/STRELA_WORD_SIZE) {
        return false;
    }

    uint32_t *mem_cfg = ctx->base + CONFIG_OFFSET;
    uint32_t *mem_inp = ctx->base + INPUT_OFFSET;
    const uint32_t *mem_out = ctx->base + OUTPUT_OFFSET;

    memset(ctx->base, 0, STRELA_DATA_REGION_SIZE);

    memcpy(mem_cfg, relu_kernel, sizeof relu_kernel);
    memcpy(mem_inp, input, sizeof *input * len);

    uint32_t len1 = len/2;
    uint32_t len2 = len1 + len%2;

    struct strela_ctrl ctrl = {
        .conf_offset = CONFIG_OFFSET,
        .conf_count = KRNL_SIZE,

        // .inp0_offset = INPUT_OFFSET + len1*0, .inp0_count = len1, .inp0_stride = STRELA_WORD_SIZE,
        .inp3_offset = INPUT_OFFSET + len1*1, .inp3_count = len2, .inp3_stride = STRELA_WORD_SIZE,

        // .out0_offset = OUTPUT_OFFSET + len1*0, .out0_count = len1,
        .out3_offset = OUTPUT_OFFSET + len1*1, .out3_count = len2,
    };

    if (ioctl(ctx->fd, IOCTL_STRELA_CONTROL, &ctrl) == -1) {
        perror("IOCTL_STRELA_CONTROL");
        return false;
    }

    if (ioctl(ctx->fd, IOCTL_STRELA_CONFIG) == -1) {
        perror("IOCTL_STRELA_CONFIG");
        return false;
    }

    if (ioctl(ctx->fd, IOCTL_STRELA_EXEC) == -1) {
        perror("IOCTL_STRELA_EXEC");
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

static
int test_device(const char *dev_path) {

    strela_ctx ctx = {0};
    if (strela_ctx_init(&ctx, dev_path) == -1) {
        fprintf(stderr, "Cannot initialize STRELA context\n");
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

        if (!strela_bypass(&ctx, input, output, BUFF_SIZE)) {
            fprintf(stderr, "Can't execute bypass kernel\n");
            return 1;
        }

        inspect_mem("input", input, BUFF_SIZE);
        inspect_mem("output", output, BUFF_SIZE);
        inspect_mem("output_ref", output_ref, BUFF_SIZE);
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

        if (!strela_relu(&ctx, input, output, BUFF_SIZE)) {
            fprintf(stderr, "Can't execute relu kernel\n");
            return 1;
        }

        inspect_mem("input", input, BUFF_SIZE);
        inspect_mem("output", output, BUFF_SIZE);
        inspect_mem("output_ref", output_ref, BUFF_SIZE);
        if (memcmp(output, output_ref, sizeof output) != 0) {
            fprintf(stderr, "relu kernel made mistakes\n");
            errors++;
        } else {
            printf("ReLU pass\n");
        }

    }

    return errors != 0;
}

int main(void) {
    test_device("/dev/strela0");
    printf("------------------\n");
    test_device("/dev/strela1");
    return 0;
}
