/* The Linux kernel documentation is a bit scattered all over the place. Here I
 * try to gather some useful pointer for future reference.
 *
 * Module entry and exit points are documented here.
 * https://docs.kernel.org/driver-api/basics.html#driver-entry-and-exit-points
 *
 * devm_ API is documented here, together with the lower level devres_ API
 * https://docs.kernel.org/driver-api/basics.html#device-resource-management
 *
 * The DMA API is documented here
 * https://docs.kernel.org/core-api/dma-api.html
 *
 * https://docs.kernel.org/driver-api/infrastructure.html#c.device
 * https://docs.kernel.org/driver-api/infrastructure.html#c.class
 * https://docs.kernel.org/core-api/kernel-api.html#char-devices
 * https://docs.kernel.org/core-api/idr.html#ida-usage
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/random.h>

#include "cgra_regs.h"
#include "cgra_dma.h"

#if 0
static size_t dma_alloc_size = CGRA_DATA_REGION_SIZE;
module_param(dma_alloc_size, size_t, 0644);
MODULE_PARM_DESC(dma_alloc_size, "Allocation size for DMA area.");
#endif

static const struct of_device_id dev_ids[] = {
	{ .compatible = "xlnx,cgra-axi-lite-1.0"},
	{},
};
MODULE_DEVICE_TABLE(of, dev_ids);

static struct class *strela_class;
static dev_t strela_base_dev_num;
static DEFINE_IDA(strela_ida);
#define MAX_STRELA_NUM 4

struct strela_data {
	struct device *dev; // TODO: rename to physical_dev also it can be removed since it can be retrieved form the parent pointer
	void __iomem *base_addr;
	dma_addr_t dma_addr;
	struct page *dma_page;

	int minor; // TODO: remove this that is already contained in dev_num
	dev_t dev_num;
	struct cdev cdev;
	struct device *device; // TODO: rename to logical_dev
};

static int
strela_open(struct inode *inode, struct file *file) {
	struct strela_data *priv = container_of(inode->i_cdev, struct strela_data, cdev);

	file->private_data = priv;

	return 0;
}

static int
strela_release(struct inode *inode, struct file *file) {
	return 0;
}

static long
strela_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param) {
	long ret = 0;
	struct strela_data *priv = file->private_data;
	struct device *dev = priv->dev;
	void __iomem *base_addr = priv->base_addr;
	dma_addr_t dma_addr = priv->dma_addr;

	switch (ioctl_num) {
		case IOCTL_CGRA_CONTROL: {
			CGRA_control_t cgra_ctrl;

			if (copy_from_user(&cgra_ctrl, (void __user *)ioctl_param, sizeof cgra_ctrl)) {
				dev_err(priv->device, "Copy from user failed");
				ret = -EFAULT;
				break;
			}

			// NOTE: maybe in this context it is better to use writel_relaxed
			// and conclude with a writel at the end to flush everything.
			writel(dma_addr + cgra_ctrl.conf_offs,      base_addr + CGRA_REG_CONF_ADDR);
			writel(cgra_ctrl.conf_count*CGRA_WORD_SIZE, base_addr + CGRA_REG_CONF_SIZE);

			writel(dma_addr + cgra_ctrl.in0_offs*CGRA_WORD_SIZE,                      base_addr + CGRA_REG_INP0_ADDR);
			writel(CGRA_PACK_STRIDE_COUNT(cgra_ctrl.in0_stride, cgra_ctrl.in0_count), base_addr + CGRA_REG_INP0_SIZE);
			writel(dma_addr + cgra_ctrl.in1_offs*CGRA_WORD_SIZE,                      base_addr + CGRA_REG_INP1_ADDR);
			writel(CGRA_PACK_STRIDE_COUNT(cgra_ctrl.in1_stride, cgra_ctrl.in1_count), base_addr + CGRA_REG_INP1_SIZE);
			writel(dma_addr + cgra_ctrl.in2_offs*CGRA_WORD_SIZE,                      base_addr + CGRA_REG_INP2_ADDR);
			writel(CGRA_PACK_STRIDE_COUNT(cgra_ctrl.in2_stride, cgra_ctrl.in2_count), base_addr + CGRA_REG_INP2_SIZE);
			writel(dma_addr + cgra_ctrl.in3_offs*CGRA_WORD_SIZE,                      base_addr + CGRA_REG_INP3_ADDR);
			writel(CGRA_PACK_STRIDE_COUNT(cgra_ctrl.in3_stride, cgra_ctrl.in3_count), base_addr + CGRA_REG_INP3_SIZE);

			writel(dma_addr + cgra_ctrl.out0_offs*CGRA_WORD_SIZE,  base_addr + CGRA_REG_OUT0_ADDR);
			writel(           cgra_ctrl.out0_count*CGRA_WORD_SIZE, base_addr + CGRA_REG_OUT0_SIZE);
			writel(dma_addr + cgra_ctrl.out1_offs*CGRA_WORD_SIZE,  base_addr + CGRA_REG_OUT1_ADDR);
			writel(           cgra_ctrl.out1_count*CGRA_WORD_SIZE, base_addr + CGRA_REG_OUT1_SIZE);
			writel(dma_addr + cgra_ctrl.out2_offs*CGRA_WORD_SIZE,  base_addr + CGRA_REG_OUT2_ADDR);
			writel(           cgra_ctrl.out2_count*CGRA_WORD_SIZE, base_addr + CGRA_REG_OUT2_SIZE);
			writel(dma_addr + cgra_ctrl.out3_offs*CGRA_WORD_SIZE,  base_addr + CGRA_REG_OUT3_ADDR);
			writel(           cgra_ctrl.out3_count*CGRA_WORD_SIZE, base_addr + CGRA_REG_OUT3_SIZE);

			// Maybe unnecessary...
			writel(1, base_addr + CGRA_REG_OUT_ARB_HOLD);
			break;
		}
		case IOCTL_CGRA_CONFIG: {
			// FLUSH_D_CACHE();
			dma_sync_single_for_device(dev, dma_addr, CGRA_DATA_REGION_SIZE, DMA_TO_DEVICE);

			writel(CGRA_CMD_CLEAR_STATE,  base_addr + CGRA_REG_CTRL);
			writel(CGRA_CMD_CLEAR_CONFIG, base_addr + CGRA_REG_CTRL);
			writel(1,                     base_addr + CGRA_REG_RESET_DMA);

			writel(CGRA_CMD_LOAD_CONFIG, base_addr + CGRA_REG_CTRL);

			u32 val = 0;
			ret = readl_poll_timeout(
				base_addr + CGRA_REG_CTRL,
				val, (val & CGRA_CMD_DONE_CONFIG), 10, 5000
			);
			break;
		}
		case IOCTL_CGRA_EXEC: {
			// FLUSH_D_CACHE();
			dma_sync_single_for_device(dev, dma_addr, CGRA_DATA_REGION_SIZE, DMA_TO_DEVICE);
			writel(CGRA_CMD_START_EXEC, base_addr + CGRA_REG_CTRL);

			u32 val = 0;
			ret = readl_poll_timeout(
				base_addr + CGRA_REG_CTRL,
				val, (val & CGRA_CMD_DONE_EXEC), 10, 500000
			);
			// INVAL_D_CACHE();
			dma_sync_single_for_cpu(dev, dma_addr, CGRA_DATA_REGION_SIZE, DMA_FROM_DEVICE);
			break;
		}
		default: {
			ret = -ENOTTY;
			break;
		}
	}

	return ret;
}

static int
strela_mmap(struct file *file, struct vm_area_struct *vma) {
	int ret = 0;
	struct strela_data *priv = file->private_data;
	struct device *dev = priv->dev;
	unsigned long requested_size = vma->vm_end - vma->vm_start;

	if (requested_size != CGRA_DATA_REGION_SIZE) {
		return -EINVAL;
	}

	ret = dma_mmap_pages(dev, vma, CGRA_DATA_REGION_SIZE, priv->dma_page);

	if (ret < 0) {
		dev_err(priv->device, "dma_mmap_pages failed with error: %d", ret);
	}

	return ret;
}

static struct file_operations strela_fops = {
	.owner          = THIS_MODULE, // https://lwn.net/Articles/31474/#:~:text=drivers%20got%20an-,owner%20field,-which%20points%20to
	.unlocked_ioctl = strela_ioctl, // https://lwn.net/Articles/119652/
	.mmap           = strela_mmap,
	.open           = strela_open,
	.release        = strela_release,
};

static int strela_probe(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	struct strela_data *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->dev = dev;

	priv->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base_addr)) {
		dev_err(dev, "Failed to map AXI memory");
		return PTR_ERR(priv->base_addr);
	}

	{
		u32 challenge1 = get_random_u32();
		u32 challenge2 = get_random_u32();
		writel(challenge1, priv->base_addr + CGRA_REG_OPA);
		writel(challenge2, priv->base_addr + CGRA_REG_OPB);
		u32 response = readl(priv->base_addr + CGRA_REG_OPR);

		if (response != challenge1 + challenge2) {
			dev_err(dev, "Adder challenge failed");
			return -EIO;
		}
	}

	priv->dma_page = dma_alloc_pages(
		dev, CGRA_DATA_REGION_SIZE, &priv->dma_addr, DMA_BIDIRECTIONAL, GFP_KERNEL
	);
	if (!priv->dma_page) {
		dev_err(dev, "Unable to allocate DMA region");
		return -ENOMEM;
	}

	// grep strela /proc/devices see dynamically allocated major number
	priv->minor = ida_alloc_max(&strela_ida, MAX_STRELA_NUM - 1, GFP_KERNEL);
	if (priv->minor < 0) {
		dev_err(dev, "No minor numbers available\n");
		ret = priv->minor;
		goto free_dma;
	}
	priv->dev_num = MKDEV(MAJOR(strela_base_dev_num), priv->minor);

	cdev_init(&priv->cdev, &strela_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->dev_num, 1);
	if (ret < 0) {
		dev_err(dev, "Failed to add cdev\n");
		goto free_ida;
	}

	// ls /dev/strela%d
	priv->device = device_create(strela_class, dev, priv->dev_num, priv, "strela%d", priv->minor);
	if (IS_ERR(priv->device)) {
		ret = PTR_ERR(priv->device);
		goto delete_cdev;
	}

	dev_info(dev, "STRELA instance %d loaded!\n", priv->minor);
	return 0;

delete_cdev:
	cdev_del(&priv->cdev);
free_ida:
	ida_free(&strela_ida, priv->minor);
free_dma:
	dma_free_pages(dev, CGRA_DATA_REGION_SIZE, priv->dma_page, priv->dma_addr, DMA_BIDIRECTIONAL);
	return ret;
}

static void strela_remove(struct platform_device *pdev) {
	struct strela_data *priv = platform_get_drvdata(pdev);

	device_destroy(strela_class, priv->dev_num);
	cdev_del(&priv->cdev);
	ida_free(&strela_ida, priv->minor);
	dma_free_pages(priv->dev, CGRA_DATA_REGION_SIZE, priv->dma_page, priv->dma_addr, DMA_BIDIRECTIONAL);

	dev_info(priv->dev, "STRELA instance %d removed!\n", priv->minor);
}

static struct platform_driver dev_driver = {
	.probe = strela_probe,
	.remove = strela_remove,
	.driver = {
		.name = "dev_driver",
		.of_match_table = dev_ids,
	}
};

static int __init
strela_driver_init(void) {
	int ret;

	ret = alloc_chrdev_region(&strela_base_dev_num, 0, MAX_STRELA_NUM, "strela");
	if (ret < 0) {
		pr_err("STRELA: Failed to allocate chrdev region\n");
		return ret;
	}

	// ls /sys/class/strela
	strela_class = class_create("strela");
	if (IS_ERR(strela_class)) {
		pr_err("STRELA: Failed to create class\n");
		ret = PTR_ERR(strela_class);
		goto unregister_chrdev;
	}

	ret = platform_driver_register(&dev_driver);
	if (ret < 0) {
		pr_err("STRELA: Failed to register platform driver\n");
		goto destroy_class;
	}

	pr_info("STRELA: Global driver initialized\n");
	return 0;

destroy_class:
	class_destroy(strela_class);
unregister_chrdev:
	unregister_chrdev_region(strela_base_dev_num, MAX_STRELA_NUM);
	return ret;
}

static void __exit
strela_driver_exit(void) {
	platform_driver_unregister(&dev_driver);
	class_destroy(strela_class);
	unregister_chrdev_region(strela_base_dev_num, MAX_STRELA_NUM);
	pr_info("STRELA: Global driver removed\n");
}

module_init(strela_driver_init);
module_exit(strela_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Bellani");
MODULE_DESCRIPTION("STRELA platform driver.");
