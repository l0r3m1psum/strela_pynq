// SPDX-License-Identifier: GPL-2.0
/* The CGRA itself: register programming, the completion interrupt, and the
 * simulated device that stands in for both when there is no FPGA.
 *
 * Nothing here decides what runs or when; that is strela_job.c. This file is
 * only the backend it runs on. */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_print.h>

#include "strela_core.h"
#include "strela_device.h"
#include "strela_job.h"
#include "strela_registers.h"

/* How long a simulated exec phase takes; the config phase takes a quarter of
 * it. Tests load the module with a large value to observe queueing, or with one
 * past STRELA_JOB_TIMEOUT_MS to exercise the timeout path. */
static unsigned int sim_job_delay_us = 200;
module_param(sim_job_delay_us, uint, 0444);
MODULE_PARM_DESC(sim_job_delay_us, "Simulated exec phase duration in microseconds");

void strela_hw_start(struct strela_device *sdev,
			    enum strela_job_phase phase)
{
	lockdep_assert_held(&sdev->lock);

	if (sdev->is_sim) {
		if (phase == STRELA_JOB_PHASE_CONFIG) {
			WRITE_ONCE(sdev->sim_abort, false);
			queue_work(system_unbound_wq, &sdev->sim_work);
		}
		return;
	}

	switch (phase) {
	case STRELA_JOB_PHASE_CONFIG:
		writel(STRELA_CMD_CLEAR_CONFIG, sdev->base + STRELA_REG_CTRL);
		writel(STRELA_RESET_DMA_PULSE, sdev->base + STRELA_REG_RESET_DMA);
		writel(STRELA_CMD_LOAD_CONFIG, sdev->base + STRELA_REG_CTRL);
		break;
	case STRELA_JOB_PHASE_EXEC:
		writel(STRELA_CMD_CLEAR_STATE, sdev->base + STRELA_REG_CTRL);
		writel(STRELA_CMD_START_EXEC, sdev->base + STRELA_REG_CTRL);
		break;
	}
}

static irqreturn_t strela_irq_handler(int irq, void *data)
{
	struct strela_device *sdev = data;
	u32 status = readl(sdev->base + STRELA_REG_CTRL);

	if (status & STRELA_CMD_PENDING_INT_CONFIG) {
		writel(STRELA_CMD_CLEAR_INT_CONFIG, sdev->base + STRELA_REG_CTRL);
		sdev->irq_status = status;
		return IRQ_WAKE_THREAD;
	} else if (status & STRELA_CMD_PENDING_INT_EXEC) {
		writel(STRELA_CMD_CLEAR_INT_EXEC, sdev->base + STRELA_REG_CTRL);
		sdev->irq_status = status;
		return IRQ_WAKE_THREAD;
	}

	return IRQ_NONE;
}

static irqreturn_t strela_irq_thread_fn(int irq, void *data)
{
	struct strela_device *sdev = data;
	u32 status = sdev->irq_status;

	sdev->irq_status = 0;

	if (status & STRELA_CMD_PENDING_INT_CONFIG) {
		strela_job_phase_done(sdev, STRELA_JOB_PHASE_CONFIG);
		return IRQ_HANDLED;
	} else if (status & STRELA_CMD_PENDING_INT_EXEC) {
		strela_job_phase_done(sdev, STRELA_JOB_PHASE_EXEC);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}


/* Byte address of a binding inside its buffer object. */
static dma_addr_t strela_binding_addr(const struct strela_binding *b)
{
	struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(b->obj);

	return dma_obj->dma_addr + (dma_addr_t)b->offset * STRELA_WORD_SIZE;
}

/* The address/size register pair of one column. */
struct strela_binding_regs {
	u32 addr;
	u32 size;
};

static void strela_hw_write_binding(struct strela_device *sdev,
				    const struct strela_binding *b,
				    struct strela_binding_regs regs)
{
	/* NOTE(Diego): max(b->stride, 1) is an hack to make it work with the
	 * config binding wich does not support stride and should always be 0.
	 */
	u32 size = ((b->stride*STRELA_WORD_SIZE) << 16)
		| max(b->stride, 1)*b->count*STRELA_WORD_SIZE;
	dma_addr_t addr = b->obj ? strela_binding_addr(b) : 0;

	drm_WARN_ON(&sdev->drm, upper_32_bits(addr));

	writel(addr, sdev->base + regs.addr);
	writel(b->obj ? size : 0, sdev->base + regs.size);
}

static const struct strela_binding_regs conf_regs = {
	STRELA_REG_CONF_ADDR, STRELA_REG_CONF_SIZE
};
static const struct strela_binding_regs inp_regs[] = {
	{ STRELA_REG_INP0_ADDR, STRELA_REG_INP0_SIZE },
	{ STRELA_REG_INP1_ADDR, STRELA_REG_INP1_SIZE },
	{ STRELA_REG_INP2_ADDR, STRELA_REG_INP2_SIZE },
	{ STRELA_REG_INP3_ADDR, STRELA_REG_INP3_SIZE },
};
static const struct strela_binding_regs out_regs[] = {
	{ STRELA_REG_OUT0_ADDR, STRELA_REG_OUT0_SIZE },
	{ STRELA_REG_OUT1_ADDR, STRELA_REG_OUT1_SIZE },
	{ STRELA_REG_OUT2_ADDR, STRELA_REG_OUT2_SIZE },
	{ STRELA_REG_OUT3_ADDR, STRELA_REG_OUT3_SIZE },
};

static_assert(ARRAY_SIZE(inp_regs) == STRELA_NUM_IO_COLS);
static_assert(ARRAY_SIZE(out_regs) == STRELA_NUM_IO_COLS);

/* Sleep out a phase, or a slice of one. Returns true if it ran its course:
 * -ETIME means the timer won, 0 means sim_abort went true under us. */
static bool strela_sim_wait(struct strela_device *sdev, u64 us)
{
	return wait_event_hrtimeout(sdev->sim_waitq, READ_ONCE(sdev->sim_abort),
				    ns_to_ktime(us * NSEC_PER_USEC)) == -ETIME;
}

/* The exec phase lasts sim_job_delay_us, the config phase a quarter of it, both
 * with up to 50% jitter so completion order is not accidentally deterministic. */
static u64 strela_sim_phase_us(enum strela_job_phase phase)
{
	u64 us = READ_ONCE(sim_job_delay_us);

	if (phase == STRELA_JOB_PHASE_CONFIG)
		us /= 4;
	if (!us)
		us = 1;

	return us + get_random_u32_below(us / 2 + 1);
}

/* One column pair of the bypass kernel: input i copied to output i. */
static void strela_sim_bypass_column(struct strela_device *sdev,
				     const struct strela_binding *in,
				     const struct strela_binding *out)
{
	struct drm_gem_dma_object *in_obj, *out_obj;
	u32 stride = max(in->stride, 1u);
	const u32 *src;
	u32 *dst;
	u32 i, n;

	/* Disabled at one end or the other: nothing to copy. */
	if (!in->obj || !out->obj)
		return;

	/* Both bindings were checked against their buffers at submit time, so
	 * the shorter of the two counts is in range for both. */
	n = min(in->count, out->count);
	if (!n)
		return;

	in_obj = to_drm_gem_dma_obj(in->obj);
	out_obj = to_drm_gem_dma_obj(out->obj);
	if (drm_WARN_ON_ONCE(&sdev->drm, !in_obj->vaddr || !out_obj->vaddr))
		return;

	/* Sync at each end the way the DMA engine's transfer would: take the
	 * input as the device would see it, publish the output the same way.
	 * Userspace cleaned the input before submitting and will invalidate the
	 * output once the fence signals; these are the two halves in between.
	 *
	 * Both are no-ops under QEMU, so this is not for the test targets. It is
	 * for the simulator running on the PYNQ itself — insmod with
	 * sim_dev_count to exercise the IREE stack without the CGRA — where the
	 * caches are real and the copy would otherwise hand back stale data.
	 * Legal now only because strela_gem_create() allocates
	 * DMA_BIDIRECTIONAL. */
	dma_sync_single_for_cpu(sdev->drm.dev, strela_binding_addr(in),
				(size_t)n * stride * STRELA_WORD_SIZE,
				DMA_FROM_DEVICE);

	src = (const u32 *)in_obj->vaddr + in->offset;
	dst = (u32 *)out_obj->vaddr + out->offset;

	/* One word every stride words, which is what the high half of the size
	 * register asks an input column to do (STRELA_MKINPSIZE). Outputs are
	 * contiguous: strela_hw_write_binding() passes strided = false. */
	for (i = 0; i < n; i++)
		dst[i] = src[i * stride];

	dma_sync_single_for_device(sdev->drm.dev, strela_binding_addr(out),
				   (size_t)n * STRELA_WORD_SIZE,
				   DMA_TO_DEVICE);
}

/* The simulated device, running one whole job: configure, then execute.
 *
 * Safe to keep using `job` after the unlock because a job is only freed once
 * its fence is signalled, and every path that signals one out from under us
 * calls strela_hw_stop() first. */
static void strela_sim_work(struct work_struct *work)
{
	struct strela_device *sdev =
		container_of(work, struct strela_device, sim_work);
	struct strela_job *job;
	unsigned long flags;
	u64 slice_us;
	int i;

	spin_lock_irqsave(&sdev->lock, flags);
	job = sdev->active;
	spin_unlock_irqrestore(&sdev->lock, flags);
	if (!job)
		return;

	if (!strela_sim_wait(sdev, strela_sim_phase_us(STRELA_JOB_PHASE_CONFIG)))
		return;
	strela_job_phase_done(sdev, STRELA_JOB_PHASE_CONFIG);

	slice_us = strela_sim_phase_us(STRELA_JOB_PHASE_EXEC) / STRELA_NUM_IO_COLS;
	for (i = 0; i < STRELA_NUM_IO_COLS; i++) {
		/* On real hardware columns do DMA in parallel bu this is good enough. */
		if (!strela_sim_wait(sdev, slice_us))
			return;
		strela_sim_bypass_column(sdev, &job->inputs[i], &job->outputs[i]);
	}

	strela_job_phase_done(sdev, STRELA_JOB_PHASE_EXEC);
}

void strela_hw_program(struct strela_device *sdev,
			      struct strela_job *job)
{
	int i;

	lockdep_assert_held(&sdev->lock);

	/* TODO(Diego): this is very bad. The binding for config and outputs
	 * should be of a different type i.e. without stride. Right now this is
	 * a patch that makes tests fail. */
	drm_WARN_ON(&sdev->drm, job->config.stride);
	for (i = 0; i < STRELA_NUM_IO_COLS; i++)
		drm_WARN_ON(&sdev->drm, job->outputs[i].stride);

	if (sdev->is_sim)
		return;

	strela_hw_write_binding(sdev, &job->config, conf_regs);

	for (i = 0; i < STRELA_NUM_IO_COLS; i++) {
		strela_hw_write_binding(sdev, &job->inputs[i], inp_regs[i]);
		strela_hw_write_binding(sdev, &job->outputs[i], out_regs[i]);
	}

	writel(STRELA_OUT_ARB_HOLD_ENABLE, sdev->base + STRELA_REG_OUT_ARB_HOLD);
}

void strela_hw_stop(struct strela_device *sdev)
{
	struct strela_job *active;

	might_sleep();
	lockdep_assert_not_held(&sdev->lock);
	active = READ_ONCE(sdev->active);
	drm_WARN_ON(&sdev->drm, active);

	if (sdev->is_sim) {
		WRITE_ONCE(sdev->sim_abort, true);
		wake_up(&sdev->sim_waitq);
		cancel_work_sync(&sdev->sim_work);
		return;
	}

	writel(STRELA_CMD_CLEAR_CONFIG, sdev->base + STRELA_REG_CTRL);
	writel(STRELA_RESET_DMA_PULSE, sdev->base + STRELA_REG_RESET_DMA);
	writel(STRELA_CMD_CLEAR_STATE, sdev->base + STRELA_REG_CTRL);
}

int strela_hw_setup(struct strela_device *sdev,
			   struct platform_device *pdev)
{
	u32 a, b;
	int irq, ret;

	if (sdev->is_sim) {
		init_waitqueue_head(&sdev->sim_waitq);
		INIT_WORK(&sdev->sim_work, strela_sim_work);
		return 0;
	}

	sdev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sdev->base))
		return PTR_ERR(sdev->base);

	a = get_random_u32();
	b = get_random_u32();
	writel(a, sdev->base + STRELA_REG_OPA);
	writel(b, sdev->base + STRELA_REG_OPB);
	if (readl(sdev->base + STRELA_REG_OPR) != a + b)
		return dev_err_probe(&pdev->dev, -EIO, "adder challenge failed\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(&pdev->dev, irq,
					strela_irq_handler,
					strela_irq_thread_fn,
					IRQF_ONESHOT | IRQF_SHARED,
					dev_name(&pdev->dev), sdev);
	if (ret)
		return ret;

	return 0;
}
