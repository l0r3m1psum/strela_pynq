#include "strela.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t bypass_kernel[STRELA_KERNEL_SIZE] = {
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

static const uint32_t relu_kernel[STRELA_KERNEL_SIZE] = {
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

static void
inspect_mem(const char *name, strela_word *mem, size_t len) {
    printf("%s: ", name);
    for (size_t i = 0; i < len; i++) printf("%d ", mem[i]);
    puts("");
}

static strela_word
max(strela_word a, strela_word b) { return a > b ? a : b; }

static bool
test_device(unsigned which) {
    int errors = 0;
    size_t len = 10;
    strela_dev *dev = strela_dev_init(which);

    // The function is idempotent.
    dev = strela_dev_init(which);

    printf("Testing STRELA %d\n", which);

    if (strela_dev_ok(dev)) {
        for (int i = 0; i < 1000; i++) {
            strela_kernel kernel = strela_kernel_get(dev);
            strela_buffer input = strela_buffer_alloc(dev, 1000);
        }
        if (strela_dev_get_err(dev).errnum != -STRELA_ERR_NO_MEM) {
            fprintf(stderr, "This test should have exhausted memory\n");
        }
        strela_dev_deinit(dev);
        dev = strela_dev_init(which);
    }

    // NOTE: strela_buffer and strela_kernel could have a pointer to the context
    // that created them to make the API cleaner... This would require getting
    // the strela_dev * also from non valid buffers and kernels which I do not
    // know how I feel about because I would need to always initialize the .dev
    // field.

    {
        strela_kernel kernel = strela_kernel_get(dev);
        strela_kernel_set(dev, kernel, bypass_kernel);
        strela_buffer input = strela_buffer_alloc(dev, len);
        strela_buffer output = strela_buffer_alloc(dev, len);
        strela_word *output_ref = malloc(sizeof *output_ref * len);

        if (output_ref && strela_dev_ok(dev)) {
            strela_word *input_ptr = strela_buffer_ptr(dev, input);
            strela_word *output_ptr = strela_buffer_ptr(dev, output);
            for (size_t i = 0; i < len; i++) {
                input_ptr[i] = i;
            }
            memset(output_ptr, 0, sizeof *output_ptr * len);
            memcpy(output_ref, strela_buffer_ptr(dev, input), sizeof *output_ref * len);
        }

        size_t len1 = len/4;
        size_t len2 = len1 + len%4;
        // Here we do not care if input or output are valid or not since if we
        // calculate junk values because one of the two buffers is not valid it
        // means that the context is not okay and the strela_config function will
        // ignore the data.
        strela_conf conf = {
            .inp0_offset = input.offset_words_from_base + len1*0, .inp0_count = len1, .inp0_stride = sizeof (strela_word),
            .inp1_offset = input.offset_words_from_base + len1*1, .inp1_count = len1, .inp1_stride = sizeof (strela_word),
            .inp2_offset = input.offset_words_from_base + len1*2, .inp2_count = len1, .inp2_stride = sizeof (strela_word),
            .inp3_offset = input.offset_words_from_base + len1*3, .inp3_count = len2, .inp3_stride = sizeof (strela_word),

            .out0_offset = output.offset_words_from_base + len1*0, .out0_count = len1,
            .out1_offset = output.offset_words_from_base + len1*1, .out1_count = len1,
            .out2_offset = output.offset_words_from_base + len1*2, .out2_count = len1,
            .out3_offset = output.offset_words_from_base + len1*3, .out3_count = len2,
        };

        strela_config(dev, kernel, &conf);
        // Not necessary to free here but this is the earliest time that it is
        // safe to do.
        strela_kernel_put(dev, kernel);
        // There is no problem executing multiple times with the same configuration.
        strela_execute(dev);
        strela_execute(dev);
        // Not necessary to free here but this is the earliest time that it is
        // safe to do.
        strela_buffer_free(dev, input);

        if (output_ref && strela_dev_ok(dev)) {
            printf("Results of bypass kernel:\n");
            inspect_mem("input", strela_buffer_ptr(dev, input), len);
            inspect_mem("output", strela_buffer_ptr(dev, output), len);
            inspect_mem("output_ref", output_ref, len);
            if (memcmp(strela_buffer_ptr(dev, output), output_ref, sizeof output) != 0) {
                fprintf(stderr, "bypass made mistakes\n");
                errors++;
            } else {
                printf("bypass pass!\n");
            }
        } else {
            fprintf(stderr, "Could not execute bypass because: %d\n", strela_dev_get_err(dev).errnum);
            errors++;
        }

        strela_buffer_free(dev, output);

        strela_dev_reset_err(dev);
        // This is not necessary but it is something that the library should be
        // capable of doing.
        strela_buffer_free_all(dev);
        strela_kernel_put_all(dev);
    }

    // TODO: what happens to the library if multiple process use it?
    {
        strela_kernel kernel = strela_kernel_get(dev);
        strela_kernel_set(dev, kernel, relu_kernel);
        strela_buffer input = strela_buffer_alloc(dev, len);
        strela_buffer output = strela_buffer_alloc(dev, len);
        strela_word *output_ref = malloc(sizeof *output_ref * len);

        if (output_ref && strela_dev_ok(dev)) {
            strela_word *input_ptr = strela_buffer_ptr(dev, input);
            strela_word *output_ptr = strela_buffer_ptr(dev, output);
            memset(output_ptr, 0, sizeof *output_ptr * len);
            for (size_t i = 0; i < len; i++) {
                input_ptr[i] = i%2 == 0 ? 1 : -1;
                output_ref[i] = max(0, input_ptr[i]);
            }
        }

        size_t len1 = len/2;
        size_t len2 = len1 + len%2;

        strela_conf conf = {
#if 0
            .inp0_offset = input.offset_words_from_base + len1*0, .inp0_count = len1, .inp0_stride = sizeof (strela_word),
            .inp3_offset = input.offset_words_from_base + len1*1, .inp3_count = len2, .inp3_stride = sizeof (strela_word),

            .out0_offset = output.offset_words_from_base + len1*0, .out0_count = len1,
            .out3_offset = output.offset_words_from_base + len1*1, .out3_count = len2,
#else
            .inp0_offset = input.offset_words_from_base, .inp0_count = len, .inp0_stride = sizeof (strela_word),
            .out0_offset = output.offset_words_from_base, .out0_count = len,
#endif
        };

        strela_config(dev, kernel, &conf);
        strela_kernel_put(dev, kernel);
        strela_execute(dev);
        strela_buffer_free(dev, input);

        if (output_ref && strela_dev_ok(dev)) {
            printf("Results of relu kernel:\n");
            inspect_mem("input", strela_buffer_ptr(dev, input), len);
            inspect_mem("output", strela_buffer_ptr(dev, output), len);
            inspect_mem("output_ref", output_ref, len);
            if (memcmp(strela_buffer_ptr(dev, output), output_ref, sizeof output) != 0) {
                fprintf(stderr, "relu made mistakes\n");
                errors++;
            } else {
                printf("relu pass!\n");
            }
        } else {
            fprintf(stderr, "Could not execute relu because: %d\n", strela_dev_get_err(dev).errnum);
            errors++;
        }

        strela_buffer_free(dev, output);
        strela_kernel_put(dev, kernel);
        strela_dev_reset_err(dev);
        strela_buffer_free_all(dev);
        strela_kernel_put_all(dev);
    }

    strela_dev_deinit(dev);

    return errors == 0;
}

int main(void) {
    bool ok = false;
    unsigned count = 0;
    strela_device_count(&count);
    printf("STRELA device count: %d\n", count);
    ok |= test_device(0);
    ok |= test_device(1);
    // Purposefully testing with an nonexistent device.
    ok |= !test_device(2);
    return ok ? 0 : 1;
}
