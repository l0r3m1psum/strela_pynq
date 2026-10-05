/* SPDX-License-Identifier: GPL-2.0 */
/* The CGRA itself: registers, interrupt, and the simulated stand-in.
 *
 * Everything here is the backend a job runs on. strela_drv.c owns the
 * device's lifetime, strela_job.c owns what runs; this file owns how. */
#ifndef STRELA_CORE_H
#define STRELA_CORE_H

#include "strela_device.h"
#include "strela_job.h"

struct platform_device;

void strela_hw_program(struct strela_device *sdev, struct strela_job *job);
void strela_hw_start(struct strela_device *sdev, enum strela_job_phase phase);
void strela_hw_stop(struct strela_device *sdev);
int strela_hw_setup(struct strela_device *sdev, struct platform_device *pdev);

#endif /* STRELA_CORE_H */
