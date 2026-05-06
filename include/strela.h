#ifndef STRELA_DMA_H
#define STRELA_DMA_H
 
#include <linux/types.h>
#include <linux/ioctl.h>

struct STRELA_control {
    __u32 a;
    __u32 b;

    __u32 conf_offs;
    __u32 conf_count;

    __u32 in0_offs;
    __u32 in0_count;
    __u32 in0_stride;
    __u32 in1_offs;
    __u32 in1_count;
    __u32 in1_stride;
    __u32 in2_offs;
    __u32 in2_count;
    __u32 in2_stride;
    __u32 in3_offs;
    __u32 in3_count;
    __u32 in3_stride;

    __u32 out0_offs;
    __u32 out0_count;
    __u32 out1_offs;
    __u32 out1_count;
    __u32 out2_offs;
    __u32 out2_count;
    __u32 out3_offs;
    __u32 out3_count;
};

#define STRELA_WORD_SIZE 4
#define STRELA_DATA_REGION_SIZE (0x100000 * 4) // 4 MB

#define CG_IOCTL_MAGIC_NUM 'x'

#define IOCTL_STRELA_CONTROL _IOW(CG_IOCTL_MAGIC_NUM, 1, struct STRELA_control)
#define IOCTL_STRELA_CONFIG  _IO(CG_IOCTL_MAGIC_NUM, 2)
#define IOCTL_STRELA_EXEC    _IO(CG_IOCTL_MAGIC_NUM, 3)

#endif
