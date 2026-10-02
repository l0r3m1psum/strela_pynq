// SPDX-License-Identifier: GPL-2.0
/* Buffer objects. Contiguous because the CGRA has no scatter/gather, cached
 * because the CPU computes on them, and therefore synced by hand. */

#include <linux/dma-mapping.h>
#include <linux/slab.h>

#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>

#include "strela_drm.h"
#include "strela_device.h"
#include "strela_gem.h"
#include "strela_registers.h"

/* Must mirror the direction strela_gem_create() allocated with:
 * dma_free_noncoherent() takes one too, and dma-debug matches it against the
 * allocation's. That is also why drm_gem_dma_object_free() cannot be the .free
 * handler — it hardcodes DMA_TO_DEVICE (drm_gem_dma_helper.c:239).
 *
 * Only buffers this driver allocated reach here. An imported dma-buf is built
 * by drm_gem_dma_prime_import_sg_table(), and since the driver no longer has a
 * .gem_create_object hook that object keeps the helper's default funcs and is
 * freed by the helper's own path. */
static void strela_gem_free(struct drm_gem_object *obj)
{
	struct drm_gem_dma_object *dma_obj = to_drm_gem_dma_obj(obj);

	/* An imported dma-buf belongs to its exporter, and the helper frees it
	 * through drm_prime_gem_destroy(). It stays out of here only because
	 * the driver has no .gem_create_object hook, so prime import leaves
	 * gem_obj->funcs at the helper's defaults. Re-adding that hook would
	 * route imports here and hand someone else's pages to
	 * dma_free_noncoherent(). */
	if (drm_WARN_ON(obj->dev, obj->import_attach))
		return;

	if (dma_obj->vaddr)
		dma_free_noncoherent(obj->dev->dev, obj->size, dma_obj->vaddr,
				     dma_obj->dma_addr, DMA_BIDIRECTIONAL);

	drm_gem_object_release(obj);
	kfree(dma_obj);
}

/* The helper's drm_gem_dma_default_funcs with our own .free. It is static, so
 * it has to be repeated; the rest are public inline wrappers and none of them
 * cares about the DMA direction. */
static const struct drm_gem_object_funcs strela_gem_funcs = {
	.free = strela_gem_free,
	.print_info = drm_gem_dma_object_print_info,
	.get_sg_table = drm_gem_dma_object_get_sg_table,
	.vmap = drm_gem_dma_object_vmap,
	.mmap = drm_gem_dma_object_mmap,
	.vm_ops = &drm_gem_dma_vm_ops,
};

/* What drm_gem_dma_create() does, except that the DMA direction is ours to
 * choose.
 *
 * The helper hardcodes DMA_TO_DEVICE in its dma_alloc_noncoherent() call
 * (drm_gem_dma_helper.c:149), which declares "the device only ever reads this".
 * That is false for these buffers: the CGRA writes its output columns, and in
 * IREE's zero-copy path one dispatch's output is the next one's input, so a
 * buffer is read and written over its life. The direction belongs to the use,
 * not to the buffer, which leaves DMA_BIDIRECTIONAL as the only honest answer.
 *
 * It is not only a description. dma-debug's check_sync() waives its direction
 * checks exactly when the allocation was DMA_BIDIRECTIONAL
 * (kernel/dma/debug.c:1125); under DMA_TO_DEVICE the invalidate half of
 * strela_gem_sync_ioctl() trips both the "different direction" check and the
 * "syncs device read-only DMA memory for cpu" one.
 *
 * __drm_gem_dma_create() is static, so its object setup is open-coded here.
 * map_noncoherent keeps the buffers cached, at the price of those explicit
 * syncs: CPU-side compute on a shared buffer would crawl on the write-combine
 * default. */
static struct drm_gem_dma_object *strela_gem_create(struct drm_device *drm,
						    size_t size)
{
	struct drm_gem_dma_object *dma_obj;
	int ret;

	dma_obj = kzalloc(sizeof(*dma_obj), GFP_KERNEL);
	if (!dma_obj)
		return ERR_PTR(-ENOMEM);

	dma_obj->base.funcs = &strela_gem_funcs;
	dma_obj->map_noncoherent = true;

	ret = drm_gem_object_init(drm, &dma_obj->base, size);
	if (ret) {
		kfree(dma_obj);
		return ERR_PTR(ret);
	}

	ret = drm_gem_create_mmap_offset(&dma_obj->base);
	if (ret)
		goto err_release;

	dma_obj->vaddr = dma_alloc_noncoherent(drm->dev, size,
					       &dma_obj->dma_addr,
					       DMA_BIDIRECTIONAL,
					       GFP_KERNEL | __GFP_NOWARN);
	if (!dma_obj->vaddr) {
		ret = -ENOMEM;
		goto err_release;
	}

	return dma_obj;

err_release:
	drm_gem_object_release(&dma_obj->base);
	kfree(dma_obj);
	return ERR_PTR(ret);
}

int strela_gem_new_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file_priv)
{
	struct drm_strela_gem_new *args = data;
	struct drm_gem_dma_object *dma_obj;
	int ret;

	if (args->flags)
		return -EINVAL;
	if (!args->size || !IS_ALIGNED(args->size, STRELA_WORD_SIZE))
		return -EINVAL;

	dma_obj = strela_gem_create(drm, PAGE_ALIGN(args->size));
	if (IS_ERR(dma_obj))
		return PTR_ERR(dma_obj);

	ret = drm_gem_handle_create(file_priv, &dma_obj->base, &args->handle);
	if (!ret)
		args->offset = drm_vma_node_offset_addr(&dma_obj->base.vma_node);

	/* The handle holds its own reference. */
	drm_gem_object_put(&dma_obj->base);
	return ret;
}

/* This uses the same design as struct dma_buf_sync
 * https://docs.kernel.org/driver-api/dma-buf.html#c.dma_buf_sync
 * drivers/accel/rocket/rocket_gem.c and drivers/gpu/drm/etnaviv/etnaviv_gem.c
 * use a simlilar prep fini pattern but but with separate ioctls also their
 * chips have MMUs so we have slightly different concerns.
 * It is not necessary to pair PREP and FINI and make two syscall for each sync
 * because we don't sync on dma_resv of the BO.
 */
int strela_gem_sync_ioctl(struct drm_device *drm, void *data,
			  struct drm_file *file_priv)
{
	struct drm_strela_gem_sync *args = data;
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *obj;
	enum dma_data_direction dir;
	size_t align = dma_get_cache_alignment();
	int ret = 0;

	if (args->flags & ~DRM_STRELA_SYNC_VALID_FLAGS_MASK)
		return -EINVAL;

	if ((args->flags & DRM_STRELA_SYNC_RW) == DRM_STRELA_SYNC_RW)
		dir = DMA_BIDIRECTIONAL;
	else if (args->flags & DRM_STRELA_SYNC_WRITE)
		dir = DMA_TO_DEVICE; /* flush */
	else if (args->flags & DRM_STRELA_SYNC_READ)
		dir = DMA_FROM_DEVICE; /* invalidate */
	else
		return -EINVAL;

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	/* TODO(Diego): this is probably not safe from overflows... */
	if (args->offset > obj->size || args->length > obj->size - args->offset) {
		ret = -EINVAL;
		goto out;
	}

	if (!IS_ALIGNED(args->offset, align) || !IS_ALIGNED(args->length, align)) {
		ret = -EINVAL;
		goto out;
	}

	if (args->length == 0)
		goto out;

	dma_obj = to_drm_gem_dma_obj(obj);

	if (args->flags & DRM_STRELA_SYNC_FINI) {
		/* CPU is done writing/reading; give ownership to device */
		dma_sync_single_for_device(drm->dev,
					   dma_obj->dma_addr + args->offset,
					   args->length, dir);
	} else { /* DRM_STRELA_SYNC_PREP */
		/* CPU is starting writing/reading; take ownership from device */
		dma_sync_single_for_cpu(drm->dev,
					dma_obj->dma_addr + args->offset,
					args->length, dir);
	}

out:
	drm_gem_object_put(obj);
	return ret;
}
