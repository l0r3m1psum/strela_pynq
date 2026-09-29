/* SPDX-License-Identifier: GPL-2.0 */
/* The module: the DRM driver, the platform glue, and per-client state. */
#ifndef STRELA_DRV_H
#define STRELA_DRV_H

#include <drm/drm_file.h>
#include <drm/gpu_scheduler.h>

/* Per open file. The entity is how the scheduler tells clients apart and
 * arbitrates between them; it is a struct rather than a bare entity so that
 * anything else that turns out to be per-client has somewhere to live. */
struct strela_file_priv {
	struct drm_sched_entity entity;
};

static inline struct strela_file_priv *to_strela_file_priv(struct drm_file *file_priv)
{
	return file_priv->driver_priv;
}

#endif /* STRELA_DRV_H */
