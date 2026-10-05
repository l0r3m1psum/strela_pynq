/* SPDX-License-Identifier: GPL-2.0 */
/* Jobs: what a submit turns into, and how the scheduler runs it. */
#ifndef STRELA_JOB_H
#define STRELA_JOB_H

#include <linux/dma-fence.h>
#include <linux/types.h>

#include <drm/drm_gem.h>
#include <drm/gpu_scheduler.h>

#include "strela_drm.h"
#include "strela_device.h"

/* Job timeout. The exec phase of the old driver waited 500ms, the config phase
 * 5ms; keep the largest and let the reset path sort out the difference. */
#define STRELA_JOB_TIMEOUT_MS 500

enum strela_job_phase {
	STRELA_JOB_PHASE_CONFIG,
	/* The configuration pahse can be splitted in two with one parts that
	 * programs the CGRA with a bitstream and another one that configures
	 * the DMA engines. After execution the DMA engines may be reconfigured
	 * to run again with the same bitstream. This could happen multiple
	 * times until a big tensor has been fully processed. */
	STRELA_JOB_PHASE_EXEC,
};

struct strela_binding {
	struct drm_gem_object *obj;
	u32 offset, count, stride;
};

struct strela_job {
	struct drm_sched_job base;
	struct strela_device *sdev;

	/* Signalled by the completion interrupt. This is what run_job() hands
	 * back to the scheduler; the fence userspace waits on is the
	 * scheduler's own (base.s_fence->finished).
	 *
	 * Allocated separately, not embedded: the scheduler still holds a
	 * reference to it when free_job() frees the job. With no release op,
	 * the last put frees it through dma_fence_free(). */
	struct dma_fence *hw_fence;

	struct strela_binding config;                      /* the bitstream */
	struct strela_binding inputs[STRELA_NUM_IO_COLS];  /* DMA conf. */
	struct strela_binding outputs[STRELA_NUM_IO_COLS]; /* DMA conf. */

	enum strela_job_phase phase;
};

static inline struct strela_job *to_strela_job(struct drm_sched_job *base)
{
	return container_of(base, struct strela_job, base);
}

int strela_job_init(struct strela_device *sdev);
void strela_job_fini(struct strela_device *sdev);

void strela_job_phase_done(struct strela_device *sdev,
			   enum strela_job_phase finished_phase);

int strela_submit_ioctl(struct drm_device *drm, void *data,
			struct drm_file *file_priv);

#endif /* STRELA_JOB_H */
