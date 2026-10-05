// SPDX-License-Identifier: GPL-2.0
/* Jobs: validating a submit, handing it to the scheduler, and reporting back.
 *
 * drm_sched owns the queue, the ordering, the timeout and the job lifetime.
 * What is left here is starting a job on the hardware and saying when it is
 * done. */

#include <linux/dma-fence.h>
#include <linux/slab.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_syncobj.h>
#include <drm/gpu_scheduler.h>

#include "strela_core.h"
#include "strela_device.h"
#include "strela_drm.h"
#include "strela_drv.h"
#include "strela_job.h"
#include "strela_registers.h"

/* The size register packs stride and total byte count into 16 bits each
 * (STRELA_MKINPSIZE), so no column can move 64 KiB or more in one job.
 * TODO: confirm against the RTL; the old driver never checked this. */
#define STRELA_MAX_COLUMN_BYTES 0xFFFFu

static void strela_job_put_bindings(struct strela_job *job)
{
	int i;

	drm_gem_object_put(job->config.obj);
	for (i = 0; i < STRELA_NUM_IO_COLS; i++) {
		drm_gem_object_put(job->inputs[i].obj);
		drm_gem_object_put(job->outputs[i].obj);
	}
}

/* Called by whichever backend finished a phase: the interrupt handler for real
 * hardware, strela_sim_work() for a simulated one. */
void strela_job_phase_done(struct strela_device *sdev,
				  enum strela_job_phase finished_phase)
{
	struct strela_job *done = NULL;
	unsigned long flags;

	spin_lock_irqsave(&sdev->lock, flags);

	if (!sdev->active || sdev->active->phase != finished_phase) {
		/* Spurious or late completion. */
		spin_unlock_irqrestore(&sdev->lock, flags);
		return;
	}

	switch (finished_phase) {
	case STRELA_JOB_PHASE_CONFIG:
		/* The CGRA is configured over the data lines, so the state has
		 * to be cleared before execution starts. */
		sdev->active->phase = STRELA_JOB_PHASE_EXEC;
		strela_hw_start(sdev, STRELA_JOB_PHASE_EXEC);
		break;
	case STRELA_JOB_PHASE_EXEC:
		done = sdev->active;
		sdev->active = NULL;
		break;
	}

	spin_unlock_irqrestore(&sdev->lock, flags);

	if (done) {
		/* TODO: post-sync (invalidate) the ranges the job wrote before
		 * telling anyone it is finished. */
		dma_fence_signal(done->hw_fence);
	}
}

static const char *strela_fence_driver_name(struct dma_fence *fence)
{
	return "strela";
}

static const char *strela_fence_timeline_name(struct dma_fence *fence)
{
	return "strela-hw";
}

static const struct dma_fence_ops strela_fence_ops = {
	.get_driver_name = strela_fence_driver_name,
	.get_timeline_name = strela_fence_timeline_name,
};

static struct dma_fence *strela_run_job(struct drm_sched_job *base)
{
	struct strela_job *job = to_strela_job(base);
	struct strela_device *sdev = job->sdev;
	unsigned long flags;

	spin_lock_irqsave(&sdev->lock, flags);
	if (sdev->dying) {
		spin_unlock_irqrestore(&sdev->lock, flags);
		return ERR_PTR(-ENODEV);
	}

	/* The scheduler must not hand us a second job while one is on the device. */
	drm_WARN_ON(&sdev->drm, sdev->active);

	job->phase = STRELA_JOB_PHASE_CONFIG;
	sdev->active = job;

	/* TODO: pre-sync (flush) the ranges this job reads, so the compiler's
	 * stream.cmd.flush maps onto the job instead of a separate ioctl. */

	strela_hw_program(sdev, job);
	strela_hw_start(sdev, STRELA_JOB_PHASE_CONFIG);
	spin_unlock_irqrestore(&sdev->lock, flags);

	/* The scheduler holds this reference until the fence signals; free_job()
	 * drops ours. */
	return dma_fence_get(job->hw_fence);
}

static enum drm_gpu_sched_stat strela_timedout_job(struct drm_sched_job *base)
{
	struct strela_job *job = to_strela_job(base);
	struct strela_device *sdev = job->sdev;
	unsigned long flags;
	bool was_active;

	/* Signalling the fence below releases the credit, so park the scheduler
	 * first. A queued job that started before drm_sched_stop() would have
	 * its hardware fence dropped there, be finished with -ECANCELED by
	 * drm_sched_start() and freed while it is still sdev->active. */
	drm_sched_wqueue_stop(&sdev->sched);

	spin_lock_irqsave(&sdev->lock, flags);
	was_active = sdev->active == job;
	if (was_active)
		sdev->active = NULL;
	spin_unlock_irqrestore(&sdev->lock, flags);

	if (was_active) {
		strela_hw_stop(sdev);
		drm_err(&sdev->drm, "job %llu timed out\n", job->hw_fence->seqno);
		dma_fence_set_error(job->hw_fence, -ETIMEDOUT);
		dma_fence_signal(job->hw_fence);
	}

	/* The scheduler has already taken this job off its pending list, so the
 	 * stop/start pair is not optional. drm_sched_stop() puts it back,
 	 * notices the fence has signalled, and arranges for the job to be
 	 * freed. Failing the fence first is what makes the error reach
 	 * userspace as -ETIMEDOUT rather than the -ECANCELED the scheduler
 	 * would use. */
	drm_sched_stop(&sdev->sched, base);
	drm_sched_start(&sdev->sched);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void strela_free_job(struct drm_sched_job *base)
{
	struct strela_job *job = to_strela_job(base);

	drm_sched_job_cleanup(base);
	strela_job_put_bindings(job);
	dma_fence_put(job->hw_fence);
	kfree(job);
}

const struct drm_sched_backend_ops strela_sched_ops = {
	.run_job = strela_run_job,
	.timedout_job = strela_timedout_job,
	.free_job = strela_free_job,
};

/* Does a binding fit its buffer and the hardware's registers?
 * Returns 0 or a negative errno.
 */
static int strela_binding_check(u64 obj_size,
				const struct drm_strela_binding *b)
{
	u64 stride = max(b->stride, 1u);
	u64 span_bytes = (u64)b->count * stride * STRELA_WORD_SIZE;
	u64 end = (u64)b->offset * STRELA_WORD_SIZE + span_bytes;

	/* The size register packs the stride in the high half and the total
	 * byte count in the low half (STRELA_MKINPSIZE), so anything from
	 * 64 KiB upwards would silently corrupt the stride field. */
	if (span_bytes > STRELA_MAX_COLUMN_BYTES)
		return -EINVAL;
	if (stride * STRELA_WORD_SIZE > STRELA_MAX_COLUMN_BYTES)
		return -EINVAL;

	if (end > obj_size)
		return -EINVAL;

	return 0;
}

/* Resolve one binding and take a reference on its buffer object, which is
 * released when the job is freed. This is what stops a buffer from going away
 * under a queued job. */
static int strela_binding_get(struct drm_file *file_priv,
			      const struct drm_strela_binding *src,
			      struct strela_binding *dst, bool optional)
{
	struct drm_gem_object *obj;
	int ret;

	if (!src->handle) {
		if (!optional)
			return -EINVAL;
		memset(dst, 0, sizeof(*dst));
		return 0;
	}

	obj = drm_gem_object_lookup(file_priv, src->handle);
	if (!obj)
		return -ENOENT;

	ret = strela_binding_check(obj->size, src);
	if (ret) {
		drm_gem_object_put(obj);
		return ret;
	}

	dst->obj = obj;
	dst->offset = src->offset;
	dst->count = src->count;
	dst->stride = src->stride;
	return 0;
}

int strela_submit_ioctl(struct drm_device *drm, void *data,
			       struct drm_file *file_priv)
{
	struct strela_device *sdev = to_strela_device(drm);
	struct strela_file_priv *priv = to_strela_file_priv(file_priv);
	struct drm_strela_submit *args = data;
	struct drm_syncobj *out_syncobj;
	struct strela_job *job;
	int i, ret;
	unsigned long flags;

	if ((args->flags & ~DRM_STRELA_SUBMIT_WAIT_SYNCOBJ) || args->pad)
		return -EINVAL;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	job->sdev = sdev;

	job->hw_fence = kzalloc(sizeof(*job->hw_fence), GFP_KERNEL);
	if (!job->hw_fence) {
		ret = -ENOMEM;
		goto err_free;
	}

	spin_lock_irqsave(&sdev->lock, flags);
	dma_fence_init(job->hw_fence, &strela_fence_ops, &sdev->lock,
		       sdev->fence_context, ++sdev->fence_seqno);
	spin_unlock_irqrestore(&sdev->lock, flags);

	/* One credit each: the device runs one job at a time. */
	ret = drm_sched_job_init(&job->base, &priv->entity, 1, file_priv);
	if (ret)
		goto err_fence_put;

	if (args->flags & DRM_STRELA_SUBMIT_WAIT_SYNCOBJ) {
		/* The scheduler holds the job back until that fence signals, so
		 * a job can wait on work finished elsewhere. Point 0: binary
		 * sync objects for now, timeline points later. */
		ret = drm_sched_job_add_syncobj_dependency(&job->base, file_priv,
							   args->in_syncobj, 0);
		if (ret)
			goto err_cleanup;
	}

	ret = strela_binding_get(file_priv, &args->config, &job->config, false);
	if (ret)
		goto err_cleanup;
	for (i = 0; i < STRELA_NUM_IO_COLS; i++) {
		ret = strela_binding_get(file_priv, &args->inputs[i],
					 &job->inputs[i], true);
		if (ret)
			goto err_cleanup;

		ret = strela_binding_get(file_priv, &args->outputs[i],
					 &job->outputs[i], true);
		if (ret)
			goto err_cleanup;
	}

	/* Resolved before arming, because after arming the scheduler owns the
	 * job and the only way out is to push it. */
	out_syncobj = drm_syncobj_find(file_priv, args->out_syncobj);
	if (!out_syncobj) {
		ret = -ENOENT;
		goto err_cleanup;
	}

	drm_sched_job_arm(&job->base);
	drm_syncobj_replace_fence(out_syncobj, &job->base.s_fence->finished);
	drm_syncobj_put(out_syncobj);

	drm_sched_entity_push_job(&job->base);
	return 0;

err_cleanup:
	drm_sched_job_cleanup(&job->base);
	strela_job_put_bindings(job);
err_fence_put:
	dma_fence_put(job->hw_fence);
err_free:
	kfree(job);
	return ret;
}

#if IS_ENABLED(CONFIG_KUNIT)

#include <kunit/test.h>

struct strela_binding_case {
	const char *name;
	u64 obj_size;
	struct drm_strela_binding binding;
	int expected;
};

static const struct strela_binding_case strela_binding_cases[] = {
	{ "exact fit", 64, { .handle = 1, .offset = 0, .count = 16, .stride = 1 }, 0 },
	{ "offset to the end", 64, { .handle = 1, .offset = 8, .count = 8, .stride = 1 }, 0 },
	{ "empty count", 64, { .handle = 1, .offset = 0, .count = 0, .stride = 1 }, 0 },
	{ "stride zero is supported", 64, { .handle = 1, .offset = 0, .count = 16, .stride = 0 }, 0 },
	{ "one word past the end", 64, { .handle = 1, .offset = 0, .count = 17, .stride = 1 }, -EINVAL },
	{ "offset past the end", 64, { .handle = 1, .offset = 17, .count = 0, .stride = 1 }, -EINVAL },
	{ "strided past the end", 64, { .handle = 1, .offset = 0, .count = 16, .stride = 2 }, -EINVAL },
	{ "count overflows u32", U64_MAX, { .handle = 1, .offset = 0, .count = 0x40000000, .stride = 4 }, -EINVAL },
	{ "stride overflows u32", U64_MAX, { .handle = 1, .offset = 0, .count = 4, .stride = 0x40000000 }, -EINVAL },
	{ "largest encodable span", 0x100000, { .handle = 1, .offset = 0, .count = 0x3FFF, .stride = 1 }, 0 },
	{ "one word too large to encode", 0x100000, { .handle = 1, .offset = 0, .count = 0x4000, .stride = 1 }, -EINVAL },
};

static void strela_binding_case_desc(const struct strela_binding_case *c,
				     char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(strela_binding, strela_binding_cases,
		  strela_binding_case_desc);

static void strela_binding_check_test(struct kunit *test)
{
	const struct strela_binding_case *c = test->param_value;

	KUNIT_EXPECT_EQ_MSG(test, strela_binding_check(c->obj_size, &c->binding),
			    c->expected, "case: %s", c->name);
}

static struct kunit_case strela_test_cases[] = {
	KUNIT_CASE_PARAM(strela_binding_check_test, strela_binding_gen_params),
	{}
};

static struct kunit_suite strela_test_suite = {
	.name = "strela",
	.test_cases = strela_test_cases,
};
kunit_test_suite(strela_test_suite);

#endif /* CONFIG_KUNIT */
