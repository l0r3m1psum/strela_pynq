#ifndef STRELA_IOCTL_H
#define STRELA_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct strela_ctrl {
    __u32 conf_offset;
    __u32 conf_count;

    __u32 inp0_offset;
    __u32 inp0_count;
    __u32 inp0_stride;
    __u32 inp1_offset;
    __u32 inp1_count;
    __u32 inp1_stride;
    __u32 inp2_offset;
    __u32 inp2_count;
    __u32 inp2_stride;
    __u32 inp3_offset;
    __u32 inp3_count;
    __u32 inp3_stride;

    __u32 out0_offset;
    __u32 out0_count;
    __u32 out1_offset;
    __u32 out1_count;
    __u32 out2_offset;
    __u32 out2_count;
    __u32 out3_offset;
    __u32 out3_count;
};

#define MAX_STRELA_NUM 4

#define STRELA_WORD_SIZE 4
#define STRELA_DATA_REGION_SIZE (0x100000 * 4) // 4 MB

#define CG_IOCTL_MAGIC_NUM 'x'

#define IOCTL_STRELA_CONTROL _IOW(CG_IOCTL_MAGIC_NUM, 1, struct strela_ctrl)
#define IOCTL_STRELA_CONFIG  _IO(CG_IOCTL_MAGIC_NUM, 2)
#define IOCTL_STRELA_EXEC    _IO(CG_IOCTL_MAGIC_NUM, 3)

#endif
