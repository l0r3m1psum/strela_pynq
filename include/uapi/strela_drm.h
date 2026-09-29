/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* UAPI for the STRELA accel driver (driver/strela_drv.c).
 *
 * Shared by the driver and userspace. The version reported by
 * DRM_IOCTL_VERSION is the handshake: bump drm_driver.major when this file
 * changes incompatibly, .minor when it only grows.
 *
 * TODO: move to include/uapi/drm/strela_drm.h if the driver ever goes in-tree.
 */
#ifndef STRELA_DRM_H
#define STRELA_DRM_H

#include <drm/drm.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* Four input and four output columns. */
#define STRELA_NUM_IO_COLS 4

/* Size of a STRELA word in bytes; mirrors the old strela_ioctl.h, which this
 * UAPI replaces. All offsets and counts below are in words. */
#ifndef STRELA_WORD_SIZE
#define STRELA_WORD_SIZE 4
#endif

/* Allocate a contiguous buffer object. Returns a handle and the fake mmap
 * offset to pass to mmap(2). */
struct drm_strela_gem_new {
	__u64 size;
	__u32 flags;    /* must be 0 */
	__u32 handle;   /* out */
	__u64 offset;   /* out: mmap offset */
};

#define DRM_STRELA_SYNC_TO_DEVICE   (1u << 0) /* flush */
#define DRM_STRELA_SYNC_FROM_DEVICE (1u << 1) /* invalidate */

/* Cache maintenance for a range of a buffer object. */
struct drm_strela_gem_sync {
	__u32 handle;
	__u32 flags;    /* DRM_STRELA_SYNC_* */
	__u64 offset;
	__u64 length;
};

/* One input or output binding. Offsets and counts are in STRELA words. */
struct drm_strela_binding {
	__u32 handle;
	__u32 offset;
	__u32 count;
	__u32 stride;   /* inputs only; outputs must pass 1 */
};

/* Wait for in_syncobj before running the job. Without this flag the field is
 * ignored, which is what keeps older userspace working: the kernel zero-fills
 * the tail of a short struct. */
#define DRM_STRELA_SUBMIT_WAIT_SYNCOBJ (1u << 0)

/* Submit one config+exec job.
 *
 * Completion is reported through sync objects (DRM_IOCTL_SYNCOBJ_CREATE), not
 * through a returned file descriptor: the job's fence is stored in out_syncobj,
 * and userspace waits with DRM_IOCTL_SYNCOBJ_WAIT.
 *
 * Note that a wait only reports "signalled": it does not say whether the job
 * failed. To see that, export the sync object with DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD
 * and DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE, then read the status with
 * SYNC_IOC_FILE_INFO.
 *
 * Sync object handles are per open file. To hand a fence to another device,
 * export it with DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD and import it there with
 * DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE.
 */
struct drm_strela_submit {
	struct drm_strela_binding config;
	struct drm_strela_binding inputs[STRELA_NUM_IO_COLS];
	struct drm_strela_binding outputs[STRELA_NUM_IO_COLS];
	__u32 out_syncobj;      /* receives the completion fence */
	__u32 flags;            /* DRM_STRELA_SUBMIT_* */
	__u32 in_syncobj;       /* with DRM_STRELA_SUBMIT_WAIT_SYNCOBJ */
	__u32 pad;              /* must be 0 */
};

#define DRM_STRELA_GEM_NEW  0x00
#define DRM_STRELA_GEM_SYNC 0x01
#define DRM_STRELA_SUBMIT   0x02

#define DRM_IOCTL_STRELA_GEM_NEW                                               \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_STRELA_GEM_NEW, struct drm_strela_gem_new)
#define DRM_IOCTL_STRELA_GEM_SYNC                                              \
	DRM_IOW(DRM_COMMAND_BASE + DRM_STRELA_GEM_SYNC, struct drm_strela_gem_sync)
#define DRM_IOCTL_STRELA_SUBMIT                                                \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_STRELA_SUBMIT, struct drm_strela_submit)

#if defined(__cplusplus)
}
#endif

#endif /* STRELA_DRM_H */
