// SPDX-License-Identifier: GPL-2.0
/* The module: the DRM driver, the ioctl table, the platform glue, and the
 * simulated devices.
 *
 * TODO This module requires CONFIG_DRM_ACCEL=y and CONFIG_SYNC_FILE=y to
 * compile and this should be documented, are they also runtime checkable? */

#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/gpu_scheduler.h>

#include "strela_core.h"
#include "strela_device.h"
#include "strela_drm.h"
#include "strela_drv.h"
#include "strela_gem.h"
#include "strela_job.h"

#define STRELA_MAX_SIM_DEVS 4

static unsigned short sim_dev_count;
module_param(sim_dev_count, ushort, 0444);
MODULE_PARM_DESC(sim_dev_count, "Number of simulated STRELA devices to add (max 4)");

/* Per-client state: one scheduler entity, which is how the scheduler tells
 * clients apart and arbitrates between them. */
static int strela_open_file(struct drm_device *drm, struct drm_file *file_priv)
{
	struct strela_device *sdev = to_strela_device(drm);
	struct drm_gpu_scheduler *sched = &sdev->sched;
	struct strela_file_priv *priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	int ret;

	if (!priv)
		return -ENOMEM;

	/* One run-queue means priority 0 is the only valid one; anything else
	 * gets clamped with an error message. Priorities would need
	 * drm_sched_init() to be given more run-queues. */
	ret = drm_sched_entity_init(&priv->entity, DRM_SCHED_PRIORITY_KERNEL,
				    &sched, 1, NULL);
	if (ret) {
		kfree(priv);
		return ret;
	}

	file_priv->driver_priv = priv;
	return 0;
}

static void strela_postclose(struct drm_device *drm, struct drm_file *file_priv)
{
	struct strela_file_priv *priv = to_strela_file_priv(file_priv);

	/* Waits for this client's jobs, or kills them if the device is gone. */
	drm_sched_entity_destroy(&priv->entity);
	file_priv->driver_priv = NULL;
	kfree(priv);
}

static const struct drm_ioctl_desc strela_ioctls[] = {
	DRM_IOCTL_DEF_DRV(STRELA_GEM_NEW, strela_gem_new_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(STRELA_GEM_SYNC, strela_gem_sync_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(STRELA_SUBMIT, strela_submit_ioctl, DRM_RENDER_ALLOW),
};

DEFINE_DRM_ACCEL_FOPS(strela_fops);

static const struct drm_driver strela_drm_driver = {
	.driver_features = DRIVER_COMPUTE_ACCEL | DRIVER_GEM | DRIVER_SYNCOBJ,
	.name = "strela",
	.desc = "STRELA CGRA accelerator",
	.major = 1,
	.minor = 0,
	.ioctls = strela_ioctls,
	.num_ioctls = ARRAY_SIZE(strela_ioctls),
	.fops = &strela_fops,
	.open = strela_open_file,
	.postclose = strela_postclose,
	/* No .gem_create_object: strela_gem_create() builds our objects, and
	 * leaving the hook unset is what keeps imported dma-bufs on the
	 * helper's default funcs (see strela_gem_free). */
	DRM_GEM_DMA_DRIVER_OPS,
};

/* This function runs multiple times. Once for each real STRELA and once for
 * each simple platform device registered for simulation. */
static int strela_probe(struct platform_device *pdev)
{
	struct strela_device *sdev;
	int ret;
	bool dev_come_from_device_tree = dev_of_node(&pdev->dev);

	sdev = devm_drm_dev_alloc(&pdev->dev, &strela_drm_driver,
				  struct strela_device, drm);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);

	/* Simulated devices are registered by name from strela_init() and have
	 * no device tree node, which is what tells the two apart. */
	sdev->is_sim = !dev_come_from_device_tree;

	spin_lock_init(&sdev->lock);
	sdev->fence_context = dma_fence_context_alloc(1);
	platform_set_drvdata(pdev, sdev);

	/* 32-bit addresses: no IOMMU and no scatter/gather, so every buffer is
	 * physically contiguous and reachable by the CGRA's DMA. */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "no usable 32-bit DMA mask\n");

	ret = strela_hw_setup(sdev, pdev);
	if (ret)
		return ret;

	ret = strela_job_init(sdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "cannot start the scheduler\n");

	ret = drm_dev_register(&sdev->drm, 0);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "cannot register the accel device\n");
		strela_job_fini(sdev);
		return ret;
	}

	drm_info(&sdev->drm, "STRELA registered%s\n",
		 sdev->is_sim ? " (simulated)" : "");
	return 0;
}

static void strela_remove(struct platform_device *pdev)
{
	struct strela_device *sdev = platform_get_drvdata(pdev);

	drm_dev_unregister(&sdev->drm); /* stop new ioctls */
	strela_job_fini(sdev); /* stop the device, retire what is left */
}

static const struct of_device_id strela_of_ids[] = {
	{ .compatible = "xlnx,cgra-axi-lite-1.0" },
	{}
};
MODULE_DEVICE_TABLE(of, strela_of_ids);

static struct platform_driver strela_platform_driver = {
	.probe = strela_probe,
	.remove = strela_remove,
	.driver = {
		.name = "strela",
		.of_match_table = strela_of_ids,
	},
};

static struct platform_device *strela_sim_pdevs[STRELA_MAX_SIM_DEVS];

static void strela_sim_dev_unregister(void)
{
	int i;

	for (i = 0; i < STRELA_MAX_SIM_DEVS; i++) {
		platform_device_unregister(strela_sim_pdevs[i]);
		strela_sim_pdevs[i] = NULL;
	}
}

static int __init strela_sim_dev_register(void)
{
	int i;

	if (sim_dev_count)
		pr_warn("The CGRA is not modelled; each enabled column is "
			"copied input to output\n");

	for (i = 0; i < sim_dev_count && i < STRELA_MAX_SIM_DEVS; i++) {
		strela_sim_pdevs[i] = platform_device_register_simple("strela", i,
								     NULL, 0);
		if (IS_ERR(strela_sim_pdevs[i])) {
			int ret = PTR_ERR(strela_sim_pdevs[i]);

			strela_sim_pdevs[i] = NULL;
			strela_sim_dev_unregister();
			return ret;
		}
	}

	return 0;
}

static int __init strela_init(void)
{
	int ret;

	if (!IS_ENABLED(CONFIG_FPGA_MGR_ZYNQ_AFI_FPGA)) {
		pr_warn("The Pynq AFI bridge driver has not been compiled with"
			"the kernel you may need to manually configure some AXI"
			" FIFO Interface registers.");
	}

	ret = platform_driver_register(&strela_platform_driver);
	if (ret)
		return ret;

	ret = strela_sim_dev_register();
	if (ret) {
		platform_driver_unregister(&strela_platform_driver);
		return ret;
	}

	return 0;
}

static void __exit strela_exit(void)
{
	strela_sim_dev_unregister();
	platform_driver_unregister(&strela_platform_driver);
}

module_init(strela_init);
module_exit(strela_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Bellani");
MODULE_DESCRIPTION("STRELA compute accelerator driver (accel framework sketch).");
