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

struct platform_device;
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

	/* One job at a time: a single CTRL register means the device cannot be
	 * pipelined, so the scheduler runs with a credit limit of one.
	 *
	 * The workqueue is ours rather than the scheduler's own because
	 * drm_sched_fini() cancels work_free_job instead of running it
	 * (drm_sched_wqueue_stop). Losing queued jobs there would be fine — the
	 * device is going away — but free_job is the retirement path for jobs
	 * that already completed, and skipping it leaks each job, its fence and
	 * its GEM references. Owning the queue lets removal flush first. */
	struct drm_gpu_scheduler sched;
	struct workqueue_struct *sched_wq;

	spinlock_t lock;                /* protects active and the hw fences */
	struct strela_job *active;      /* job that is currently executing */
	bool dying;                     /* removal started; no new work */
	u64 fence_context;
	u64 fence_seqno;
};

static inline struct strela_device *to_strela_device(struct drm_device *drm)
{
	return container_of(drm, struct strela_device, drm);
}

int strela_device_init(struct strela_device *sdev, struct platform_device *pdev);
void strela_device_fini(struct strela_device *sdev);
void strela_drain(struct strela_device *sdev);

#endif /* STRELA_DEVICE_H */
