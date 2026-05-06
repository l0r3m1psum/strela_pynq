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

#include "strela_regs.h"
#include "strela.h"

#if 0
static size_t dma_alloc_size = STRELA_DATA_REGION_SIZE;
module_param(dma_alloc_size, size_t, 0644);
MODULE_PARM_DESC(dma_alloc_size, "Allocation size for DMA area.");
#endif

static const struct of_device_id dev_ids[] = {
	{ .compatible = "xlnx,cgra-axi-lite-1.0"},
	{},
};
MODULE_DEVICE_TABLE(of, dev_ids);

static struct class *strela_class;
static dev_t strela_base_dev_num; // Only the major number
static DEFINE_IDA(strela_ida);
#define MAX_STRELA_NUM 4

struct strela_data {
	void __iomem *base_addr;
	dma_addr_t dma_addr;
	struct page *dma_page;

	dev_t dev_num; // major + minor
	struct cdev cdev;
	struct device *logical_dev;
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
	struct device *physical_dev = priv->logical_dev->parent;
	void __iomem *base_addr = priv->base_addr;
	dma_addr_t dma_addr = priv->dma_addr;

	switch (ioctl_num) {
		case IOCTL_STRELA_CONTROL: {
			struct STRELA_control ctrl;

			if (copy_from_user(&ctrl, (void __user *)ioctl_param, sizeof ctrl)) {
				dev_err(priv->logical_dev, "Copy from user failed");
				ret = -EFAULT;
				break;
			}

			// NOTE: maybe in this context it is better to use writel_relaxed
			// and conclude with a writel at the end to flush everything.
			writel(dma_addr + ctrl.conf_offs,      base_addr + STRELA_REG_CONF_ADDR);
			writel(ctrl.conf_count*STRELA_WORD_SIZE, base_addr + STRELA_REG_CONF_SIZE);

			writel(dma_addr + ctrl.in0_offs*STRELA_WORD_SIZE,         base_addr + STRELA_REG_INP0_ADDR);
			writel(STRELA_MKINPSIZE(ctrl.in0_stride, ctrl.in0_count), base_addr + STRELA_REG_INP0_SIZE);
			writel(dma_addr + ctrl.in1_offs*STRELA_WORD_SIZE,         base_addr + STRELA_REG_INP1_ADDR);
			writel(STRELA_MKINPSIZE(ctrl.in1_stride, ctrl.in1_count), base_addr + STRELA_REG_INP1_SIZE);
			writel(dma_addr + ctrl.in2_offs*STRELA_WORD_SIZE,         base_addr + STRELA_REG_INP2_ADDR);
			writel(STRELA_MKINPSIZE(ctrl.in2_stride, ctrl.in2_count), base_addr + STRELA_REG_INP2_SIZE);
			writel(dma_addr + ctrl.in3_offs*STRELA_WORD_SIZE,         base_addr + STRELA_REG_INP3_ADDR);
			writel(STRELA_MKINPSIZE(ctrl.in3_stride, ctrl.in3_count), base_addr + STRELA_REG_INP3_SIZE);

			writel(dma_addr + ctrl.out0_offs*STRELA_WORD_SIZE,  base_addr + STRELA_REG_OUT0_ADDR);
			writel(           ctrl.out0_count*STRELA_WORD_SIZE, base_addr + STRELA_REG_OUT0_SIZE);
			writel(dma_addr + ctrl.out1_offs*STRELA_WORD_SIZE,  base_addr + STRELA_REG_OUT1_ADDR);
			writel(           ctrl.out1_count*STRELA_WORD_SIZE, base_addr + STRELA_REG_OUT1_SIZE);
			writel(dma_addr + ctrl.out2_offs*STRELA_WORD_SIZE,  base_addr + STRELA_REG_OUT2_ADDR);
			writel(           ctrl.out2_count*STRELA_WORD_SIZE, base_addr + STRELA_REG_OUT2_SIZE);
			writel(dma_addr + ctrl.out3_offs*STRELA_WORD_SIZE,  base_addr + STRELA_REG_OUT3_ADDR);
			writel(           ctrl.out3_count*STRELA_WORD_SIZE, base_addr + STRELA_REG_OUT3_SIZE);

			// Maybe unnecessary...
			writel(1, base_addr + STRELA_REG_OUT_ARB_HOLD);
			break;
		}
		case IOCTL_STRELA_CONFIG: {
			// FLUSH_D_CACHE();
			dma_sync_single_for_device(physical_dev, dma_addr, STRELA_DATA_REGION_SIZE, DMA_BIDIRECTIONAL); // DMA_TO_DEVICE);

			writel(STRELA_CMD_CLEAR_STATE,  base_addr + STRELA_REG_CTRL);
			writel(STRELA_CMD_CLEAR_CONFIG, base_addr + STRELA_REG_CTRL);
			writel(1,                     base_addr + STRELA_REG_RESET_DMA);

			writel(STRELA_CMD_LOAD_CONFIG, base_addr + STRELA_REG_CTRL);

			u32 val = 0;
			ret = readl_poll_timeout(
				base_addr + STRELA_REG_CTRL,
				val, (val & STRELA_CMD_DONE_CONFIG), 10, 5000
			);
			break;
		}
		case IOCTL_STRELA_EXEC: {
			// FLUSH_D_CACHE();
			dma_sync_single_for_device(physical_dev, dma_addr, STRELA_DATA_REGION_SIZE, DMA_BIDIRECTIONAL); // DMA_TO_DEVICE);
			writel(STRELA_CMD_START_EXEC, base_addr + STRELA_REG_CTRL);

			u32 val = 0;
			ret = readl_poll_timeout(
				base_addr + STRELA_REG_CTRL,
				val, (val & STRELA_CMD_DONE_EXEC), 10, 500000
			);
			// INVAL_D_CACHE();
			dma_sync_single_for_cpu(physical_dev, dma_addr, STRELA_DATA_REGION_SIZE, DMA_BIDIRECTIONAL); // DMA_FROM_DEVICE);
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
	struct device *physical_dev = priv->logical_dev->parent;
	unsigned long requested_size = vma->vm_end - vma->vm_start;

	if (requested_size != STRELA_DATA_REGION_SIZE) {
		return -EINVAL;
	}

	ret = dma_mmap_pages(physical_dev, vma, STRELA_DATA_REGION_SIZE, priv->dma_page);

	if (ret < 0) {
		dev_err(physical_dev, "dma_mmap_pages failed with error: %d", ret);
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
	struct device *physical_dev = &pdev->dev;
	struct strela_data *priv;
	int minor, ret;

	priv = devm_kzalloc(physical_dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	priv->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base_addr)) {
		dev_err(physical_dev, "Failed to map AXI memory");
		return PTR_ERR(priv->base_addr);
	}

	{
		u32 challenge1 = get_random_u32();
		u32 challenge2 = get_random_u32();
		writel(challenge1, priv->base_addr + STRELA_REG_OPA);
		writel(challenge2, priv->base_addr + STRELA_REG_OPB);
		u32 response = readl(priv->base_addr + STRELA_REG_OPR);

		if (response != challenge1 + challenge2) {
			dev_err(physical_dev, "Adder challenge failed");
			return -EIO;
		}
	}

	priv->dma_page = dma_alloc_pages(physical_dev, STRELA_DATA_REGION_SIZE, &priv->dma_addr, DMA_BIDIRECTIONAL, GFP_KERNEL);
	if (!priv->dma_page) {
		dev_err(physical_dev, "Unable to allocate DMA region");
		return -ENOMEM;
	}

	// grep strela /proc/devices see dynamically allocated major number
	minor = ida_alloc_max(&strela_ida, MAX_STRELA_NUM - 1, GFP_KERNEL);
	if (minor < 0) {
		dev_err(physical_dev, "No minor numbers available\n");
		ret = minor;
		goto free_dma;
	}
	priv->dev_num = MKDEV(MAJOR(strela_base_dev_num), minor);

	cdev_init(&priv->cdev, &strela_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->dev_num, 1);
	if (ret < 0) {
		dev_err(physical_dev, "Failed to add cdev\n");
		goto free_ida;
	}

	// ls /dev/strela%d
	priv->logical_dev = device_create(strela_class, physical_dev, priv->dev_num, priv, "strela%d", minor);
	if (IS_ERR(priv->logical_dev)) {
		ret = PTR_ERR(priv->logical_dev);
		goto delete_cdev;
	}

	dev_info(physical_dev, "STRELA instance %d loaded!\n", minor);
	return 0;

delete_cdev:
	cdev_del(&priv->cdev);
free_ida:
	ida_free(&strela_ida, minor);
free_dma:
	dma_free_pages(physical_dev, STRELA_DATA_REGION_SIZE, priv->dma_page, priv->dma_addr, DMA_BIDIRECTIONAL);
	return ret;
}

static void strela_remove(struct platform_device *pdev) {
	struct strela_data *priv = platform_get_drvdata(pdev);

	device_destroy(strela_class, priv->dev_num);
	cdev_del(&priv->cdev);
	ida_free(&strela_ida, MINOR(priv->dev_num));
	dma_free_pages(priv->logical_dev->parent, STRELA_DATA_REGION_SIZE, priv->dma_page, priv->dma_addr, DMA_BIDIRECTIONAL);
	dev_info(priv->logical_dev->parent, "STRELA instance %d removed!\n", MINOR(priv->dev_num));
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

	if (!IS_ENABLED(CONFIG_XILINX_AFI_FPGA)) {
		pr_warn("The Xilinx AFI bridge driver has not been compiled with the kernel "
			"you may need to manually configure some AXI FIFO Interface registers.");
	}

	if (!IS_ENABLED(CONFIG_FPGA_MGR_ZYNQ_AFI_FPGA)) {
		pr_warn("The Pynq AFI bridge driver has not been compiled with the kernel "
			"you may need to manually configure some AXI FIFO Interface registers.");
	}

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
