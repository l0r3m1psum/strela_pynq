#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/random.h>

#include <linux/cdev.h>
// #include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/device.h>

#include "cgra_regs.h"
#include "cgra_dma.h"

static const struct of_device_id dev_ids[] = {
	{ .compatible = "xlnx,cgra-axi-lite-1.0"},
	{},
};
MODULE_DEVICE_TABLE(of, dev_ids);

struct strela_data {
	struct device *dev;
	void __iomem *base_addr;
	dma_addr_t dma_addr;
	struct page *dma_page;

	dev_t dev_num;
	struct cdev cdev;
	struct class *class;
	struct device *device;
};

static int
strela_open(struct inode *inode, struct file *file) {
	// TODO: try_module_get(THIS_MODULE) ???
	struct strela_data *priv = container_of(inode->i_cdev, struct strela_data, cdev);
	file->private_data = priv;
	return 0;
}

static int
strela_release(struct inode *inode, struct file *file) {
	// TODO: module_put(THIS_MODULE) ???
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
				dev_err(dev, "Copy from user failed");
				ret = -ENOMEM; // TODO: correct return code?
				break;
			}

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

	ret = dma_mmap_pages(priv->dev, vma, CGRA_DATA_REGION_SIZE, priv->dma_page);

	if (ret < 0) {
		dev_err(dev, "dma_mmap_pages failed with error: %d", ret);
	}

	return ret;
}

static struct file_operations strela_fops = {
	.owner          = THIS_MODULE, // TODO: ???
	.unlocked_ioctl = strela_ioctl,
	.mmap           = strela_mmap,
	.open           = strela_open,
	.release        = strela_release,
};

static int
strela_probe(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	struct strela_data *priv = NULL;

	priv = devm_kzalloc(dev, sizeof *priv, GFP_KERNEL);
	if (!priv) {
		dev_err(dev, "Out of memory");
		return -ENOMEM;
	}

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

	int ret;

	// TODO: ???
	ret = alloc_chrdev_region(&priv->dev_num, 0, 1, "cgra_device");
	if (ret < 0)
		return ret;

	cdev_init(&priv->cdev, &strela_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->dev_num, 1);
	if (ret < 0)
		goto unregister_region;

	// TODO: ???
	priv->class = class_create("strela");
	if (IS_ERR(priv->class)) {
		ret = PTR_ERR(priv->class);
		goto delete_cdev;
	}

	// TODO: ???
	priv->device = device_create(priv->class, NULL, priv->dev_num, NULL, "cgra0");
	if (IS_ERR(priv->device)) {
		ret = PTR_ERR(priv->device);
		goto destroy_class;
	}

	dev_info(dev, "STRELA loaded!");

	return 0;

destroy_class:
	class_destroy(priv->class);
delete_cdev:
	cdev_del(&priv->cdev);
unregister_region:
	unregister_chrdev_region(priv->dev_num, 1);
	return ret;
}

static void
strela_remove(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	struct strela_data *priv = platform_get_drvdata(pdev);

	dma_free_pages(
		dev, CGRA_DATA_REGION_SIZE, priv->dma_page, priv->dma_addr, DMA_BIDIRECTIONAL
	);
	device_destroy(priv->class, priv->dev_num);
	class_destroy(priv->class);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->dev_num, 1);
	dev_info(dev, "STRELA removed!");
}

static struct platform_driver dev_driver = {
	.probe = strela_probe,
	.remove = strela_remove,
	.driver = {
		.name = "dev_driver",
		.of_match_table = dev_ids,
	}
};

module_platform_driver(dev_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Bellani");
MODULE_DESCRIPTION("STRELA platform driver.");
