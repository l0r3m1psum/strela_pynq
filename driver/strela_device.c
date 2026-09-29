// SPDX-License-Identifier: GPL-2.0
/* Device lifetime: bringing the scheduler up, and taking it down in the one
 * order that does not leak. */

#include <linux/dma-fence.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>

#include "strela_core.h"
#include "strela_device.h"
#include "strela_job.h"

/* Stop taking work and fail whatever is in flight.
 *
 * drm_sched_fini() in strela_remove() takes care of the queued jobs; this
 * handles the one on the hardware, which the scheduler cannot know about. */
void strela_drain(struct strela_device *sdev)
{
	struct strela_job *active;
	unsigned long flags;

	spin_lock_irqsave(&sdev->lock, flags);
	sdev->dying = true;
	active = sdev->active;
	if (active) {
		strela_hw_reset(sdev);
		sdev->active = NULL;
	}
	spin_unlock_irqrestore(&sdev->lock, flags);

	if (active) {
		/* Before the fence is signalled: signalling lets free_job() run,
		 * and a simulated device may still be copying out of this job. */
		strela_sim_settle(sdev);
		dma_fence_set_error(active->hw_fence, -ENODEV);
		dma_fence_signal(active->hw_fence);
	}
}

/* Bring up the queue. The scheduler owns the ordering and the timeout from
 * here on; one run-queue and one credit make it a single in-order engine. */
int strela_device_init(struct strela_device *sdev, struct platform_device *pdev)
{
	int ret;

	sdev->sched_wq = alloc_ordered_workqueue("strela-sched", 0);
	if (!sdev->sched_wq)
		return -ENOMEM;

	ret = drm_sched_init(&sdev->sched, &strela_sched_ops,
			     sdev->sched_wq, /*num_rqs=*/1, /*credit_limit=*/1,
			     /*hang_limit=*/0,
			     msecs_to_jiffies(STRELA_JOB_TIMEOUT_MS),
			     /*timeout_wq=*/NULL, /*score=*/NULL,
			     "strela", &pdev->dev);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "cannot start the scheduler\n");
		destroy_workqueue(sdev->sched_wq);
		return ret;
	}

	return 0;
}

/* The flush is not optional: drm_sched_fini() cancels work_free_job rather
 * than running it, so jobs that already completed would never be retired and
 * would take their buffers with them. See the comment on sched_wq. */
void strela_device_fini(struct strela_device *sdev)
{
	flush_workqueue(sdev->sched_wq);
	drm_sched_fini(&sdev->sched);
	destroy_workqueue(sdev->sched_wq);
}
