/* SPDX-License-Identifier: GPL-2.0 */
/* The CGRA itself: registers, interrupt, and the simulated stand-in.
 *
 * Everything here is the backend a job runs on. strela_device.c owns the
 * device's lifetime, strela_job.c owns what runs; this file owns how. */
#ifndef STRELA_CORE_H
#define STRELA_CORE_H

#include "strela_device.h"
#include "strela_job.h"

struct platform_device;

/* Called with sdev->lock held and interrupts off: these are register writes on
 * real hardware, and on a simulated device they only wake the worker. */
void strela_hw_program(struct strela_device *sdev, struct strela_job *job);
void strela_hw_start(struct strela_device *sdev, enum strela_job_phase phase);
void strela_hw_reset(struct strela_device *sdev);

/* A real reset stops the device before the write returns; the simulated one is
 * asynchronous, so callers of strela_hw_reset() finish it off with this, in
 * process context, before signalling the job's fence. */
void strela_sim_settle(struct strela_device *sdev);

int strela_hw_setup(struct strela_device *sdev, struct platform_device *pdev);

#endif /* STRELA_CORE_H */
