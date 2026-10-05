/* SPDX-License-Identifier: GPL-2.0 */
/* The STRELA device: one CGRA, one scheduler, one job at a time. */
#ifndef STRELA_DEVICE_H
#define STRELA_DEVICE_H

#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_device.h>
#include <drm/gpu_scheduler.h>

struct strela_job;

struct strela_device {
	struct drm_device drm;
	void __iomem *base;
	u32 irq_status;

	bool is_sim;

	/* Simulation: the "device" is a work item that sleeps, and sim_abort is
	 * its reset line — raised under the lock, read by the worker. */
	struct work_struct sim_work;
	wait_queue_head_t sim_waitq;
	bool sim_abort;

	struct drm_gpu_scheduler sched;

	spinlock_t lock;                /* protects active and the hw fences */
	struct strela_job *active;      /* job that is currently executing */
	u64 fence_context;
	u64 fence_seqno;
};

static inline struct strela_device *to_strela_device(struct drm_device *drm)
{
	return container_of(drm, struct strela_device, drm);
}

#endif /* STRELA_DEVICE_H */
