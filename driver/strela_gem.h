/* SPDX-License-Identifier: GPL-2.0 */
/* Buffer objects: contiguous, cached, and synced by hand. */
#ifndef STRELA_GEM_H
#define STRELA_GEM_H

#include <drm/drm_device.h>
#include <drm/drm_file.h>

int strela_gem_new_ioctl(struct drm_device *drm, void *data,
			 struct drm_file *file_priv);
int strela_gem_sync_ioctl(struct drm_device *drm, void *data,
			  struct drm_file *file_priv);

#endif /* STRELA_GEM_H */
