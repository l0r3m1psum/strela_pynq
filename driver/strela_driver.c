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
 * Bit manipulation stuff
 * https://docs.kernel.org/core-api/kernel-api.html#basic-kernel-library-functions
 *
 * https://docs.kernel.org/driver-api/infrastructure.html#c.device
 * https://docs.kernel.org/driver-api/infrastructure.html#c.class
 * https://docs.kernel.org/core-api/kernel-api.html#char-devices
 * https://docs.kernel.org/core-api/idr.html#ida-usage
 * https://docs.kernel.org/userspace-api/ioctl/ioctl-number.html
 * https://docs.kernel.org/core-api/kernel-api.html#crc-and-math-functions-in-linux
 * https://docs.kernel.org/scheduler/completion.html
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/random.h>

#include "strela_regs.h"
#include "strela_ioctl.h"

#if 0
#include <linux/moduleparam.h>
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

struct strela_data {
	void __iomem *base_addr;
	dma_addr_t dma_addr;
	struct page *dma_page;
	unsigned long in_use;

	dev_t dev_num; // major + minor
	struct cdev cdev;
	struct device *logical_dev;

	u32 irq_status;
	// NOTE: can I get away with only using one?
	struct completion conf_done;
	struct completion exec_done;
};

static int
strela_open(struct inode *inode, struct file *file) {
	struct strela_data *priv = container_of(inode->i_cdev, struct strela_data, cdev);

	if (test_and_set_bit(0, &priv->in_use)) {
		dev_warn(priv->logical_dev, "Device is already open\n");
		return -EBUSY;
	}

	file->private_data = priv;

	return 0;
}

static int
strela_release(struct inode *inode, struct file *file) {
	struct strela_data *priv = file->private_data;

	clear_bit(0, &priv->in_use);

	return 0;
}

static bool
strela_check_bounds(u32 offset_words, u32 count_words, u32 stride) {
	// offset_words*WORD_SIZE + count_words*stride*WORD_SIZE >= DATA_REGION_SIZE
	u32 offset_bytes = 0;
	u32 count_bytes = 0;
	u32 end_bytes = 0;

	if (check_mul_overflow(offset_words, (u32)STRELA_WORD_SIZE, &offset_bytes))
		return true;

	if (check_mul_overflow(count_words, stride, &count_bytes))
		return true;

	if (check_mul_overflow(count_bytes, (u32)STRELA_WORD_SIZE, &count_bytes))
		return true;

	if (check_add_overflow(offset_bytes, count_bytes, &end_bytes))
		return true;

	if (end_bytes > STRELA_DATA_REGION_SIZE)
		return true;

	return false;
}

static long
strela_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param) {
	long ret = 0;
	struct strela_data *priv = file->private_data;
	struct device *physical_dev = priv->logical_dev->parent;
	void __iomem *base_addr = priv->base_addr;
	dma_addr_t dma_addr = priv->dma_addr;

	// NOTE: is there any case in which I should reinit_completion?

	switch (ioctl_num) {
		case IOCTL_STRELA_CONTROL: {
			struct strela_ctrl ctrl;
			u32 conf_end;

			if (copy_from_user(&ctrl, (void __user *)ioctl_param, sizeof ctrl)) {
				dev_err(priv->logical_dev, "Copy from user failed");
				ret = -EFAULT;
				break;
			}

			// Probably in the future STRELA will have 32-bit configuration
			// words and 8-bit data words.

			// TODO: all this calculations should be done in user-space here
			// only alignment should be checked.

			if (
				strela_check_bounds(ctrl.conf_offset, ctrl.conf_count, 1)
				|| strela_check_bounds(ctrl.inp0_offset, ctrl.inp0_count, ctrl.inp0_stride)
				|| strela_check_bounds(ctrl.inp1_offset, ctrl.inp1_count, ctrl.inp1_stride)
				|| strela_check_bounds(ctrl.inp2_offset, ctrl.inp2_count, ctrl.inp2_stride)
				|| strela_check_bounds(ctrl.inp3_offset, ctrl.inp3_count, ctrl.inp3_stride)
				|| strela_check_bounds(ctrl.out0_offset, ctrl.out0_count, 1)
				|| strela_check_bounds(ctrl.out1_offset, ctrl.out1_count, 1)
				|| strela_check_bounds(ctrl.out2_offset, ctrl.out2_count, 1)
				|| strela_check_bounds(ctrl.out3_offset, ctrl.out3_count, 1)
			) {
				ret = -EINVAL;
				break;
			}

			// NOTE: maybe in this context it is better to use writel_relaxed
			// and conclude with a writel at the end to flush everything.
			writel(dma_addr + ctrl.conf_offset*STRELA_WORD_SIZE, base_addr + STRELA_REG_CONF_ADDR);
			writel(ctrl.conf_count*STRELA_WORD_SIZE,             base_addr + STRELA_REG_CONF_SIZE);

			writel(dma_addr + ctrl.inp0_offset*STRELA_WORD_SIZE,                         base_addr + STRELA_REG_INP0_ADDR);
			writel(STRELA_MKINPSIZE(ctrl.inp0_stride*STRELA_WORD_SIZE, ctrl.inp0_count), base_addr + STRELA_REG_INP0_SIZE);
			writel(dma_addr + ctrl.inp1_offset*STRELA_WORD_SIZE,                         base_addr + STRELA_REG_INP1_ADDR);
			writel(STRELA_MKINPSIZE(ctrl.inp1_stride*STRELA_WORD_SIZE, ctrl.inp1_count), base_addr + STRELA_REG_INP1_SIZE);
			writel(dma_addr + ctrl.inp2_offset*STRELA_WORD_SIZE,                         base_addr + STRELA_REG_INP2_ADDR);
			writel(STRELA_MKINPSIZE(ctrl.inp2_stride*STRELA_WORD_SIZE, ctrl.inp2_count), base_addr + STRELA_REG_INP2_SIZE);
			writel(dma_addr + ctrl.inp3_offset*STRELA_WORD_SIZE,                         base_addr + STRELA_REG_INP3_ADDR);
			writel(STRELA_MKINPSIZE(ctrl.inp3_stride*STRELA_WORD_SIZE, ctrl.inp3_count), base_addr + STRELA_REG_INP3_SIZE);

			writel(dma_addr + ctrl.out0_offset*STRELA_WORD_SIZE, base_addr + STRELA_REG_OUT0_ADDR);
			writel(           ctrl.out0_count*STRELA_WORD_SIZE,  base_addr + STRELA_REG_OUT0_SIZE);
			writel(dma_addr + ctrl.out1_offset*STRELA_WORD_SIZE, base_addr + STRELA_REG_OUT1_ADDR);
			writel(           ctrl.out1_count*STRELA_WORD_SIZE,  base_addr + STRELA_REG_OUT1_SIZE);
			writel(dma_addr + ctrl.out2_offset*STRELA_WORD_SIZE, base_addr + STRELA_REG_OUT2_ADDR);
			writel(           ctrl.out2_count*STRELA_WORD_SIZE,  base_addr + STRELA_REG_OUT2_SIZE);
			writel(dma_addr + ctrl.out3_offset*STRELA_WORD_SIZE, base_addr + STRELA_REG_OUT3_ADDR);
			writel(           ctrl.out3_count*STRELA_WORD_SIZE,  base_addr + STRELA_REG_OUT3_SIZE);

			// Maybe unnecessary...
			writel(1, base_addr + STRELA_REG_OUT_ARB_HOLD);
			break;
		}
		case IOCTL_STRELA_CONFIG: {
			// FLUSH_D_CACHE();
			dma_sync_single_for_device(physical_dev, dma_addr, STRELA_DATA_REGION_SIZE, DMA_BIDIRECTIONAL); // DMA_TO_DEVICE);

			writel(STRELA_CMD_CLEAR_CONFIG, base_addr + STRELA_REG_CTRL);
			writel(1,                       base_addr + STRELA_REG_RESET_DMA);

			writel(STRELA_CMD_LOAD_CONFIG, base_addr + STRELA_REG_CTRL);

			ret = wait_for_completion_interruptible_timeout(&priv->conf_done, usecs_to_jiffies(5000));
			if (ret == 0) {
				ret = -ETIMEDOUT;
			} else if (ret == -ERESTARTSYS) {
				ret = -ERESTARTSYS;
			} else {
				ret = 0;
			}

			// The CGRA is configured over the data lines hence we need to clear
			// them before starting with the execution.
			writel(STRELA_CMD_CLEAR_STATE,  base_addr + STRELA_REG_CTRL);
			break;
		}
		case IOCTL_STRELA_EXEC: {
			// FLUSH_D_CACHE();
			dma_sync_single_for_device(physical_dev, dma_addr, STRELA_DATA_REGION_SIZE, DMA_BIDIRECTIONAL); // DMA_TO_DEVICE);
			writel(STRELA_CMD_START_EXEC, base_addr + STRELA_REG_CTRL);

			ret = wait_for_completion_interruptible_timeout(&priv->exec_done, usecs_to_jiffies(500000));
			if (ret == 0) {
				ret = -ETIMEDOUT;
			} else if (ret == -ERESTARTSYS) {
				ret = -ERESTARTSYS;
			} else {
				ret = 0;
			}

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

static void
strela_vm_close(struct vm_area_struct *vma) {
	struct device *physical_dev = vma->vm_private_data;

	dev_info(physical_dev, "Memory is being unmapped");
}

static struct vm_operations_struct strela_vm_operations = {
	.close = strela_vm_close,
	// If fork will ever be supported...
	// .open = strela_vm_open,
};

// There are solutions for contiguous memory allocation from user space (e.g.
// mmap(MAP_CONTIG)) which paired with dma_map_single for kernels (which are
// never going to be bigger that a 4KiB page) could free the driver from
// handling mmap.
// https://blog.linuxplumbersconf.org/2017/ocw/system/presentations/4669/original/Support%20user%20space%20POSIX%20conformant%20contiguous__v1.00.pdf
// https://lwn.net/Articles/753167/
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

	vm_flags_set(vma, VM_DONTCOPY);

	vma->vm_ops = &strela_vm_operations;
	vma->vm_private_data = physical_dev;

	return ret;
}

static struct file_operations strela_fops = {
	.owner          = THIS_MODULE, // https://lwn.net/Articles/31474/#:~:text=drivers%20got%20an-,owner%20field,-which%20points%20to
	.unlocked_ioctl = strela_ioctl, // https://lwn.net/Articles/119652/
	.mmap           = strela_mmap,
	// No munmap https://lwn.net/Articles/1038715/
	.open           = strela_open,
	.release        = strela_release,
};

static irqreturn_t
strela_irq_handler(int irq, void *data) {
	struct strela_data *priv = data;
	u32 status = readl(priv->base_addr + STRELA_REG_CTRL);

	if (status & STRELA_CMD_PENDING_INT_CONFIG) {
		writel(STRELA_CMD_CLEAR_INT_CONFIG, priv->base_addr + STRELA_REG_CTRL);
		priv->irq_status = status;
		return IRQ_WAKE_THREAD;
	} else if (status & STRELA_CMD_PENDING_INT_EXEC) {
		writel(STRELA_CMD_CLEAR_INT_EXEC, priv->base_addr + STRELA_REG_CTRL);
		priv->irq_status = status;
		return IRQ_WAKE_THREAD;
	}
	return IRQ_NONE;
}

static irqreturn_t
strela_irq_thread_fn(int irq, void *data) {
	struct strela_data *priv = data;
	// u32 status = readl(priv->base_addr + STRELA_REG_CTRL);

	if (priv->irq_status & STRELA_CMD_PENDING_INT_CONFIG) {
		priv->irq_status = 0;
		complete(&priv->conf_done);
		return IRQ_HANDLED;
	} else if (priv->irq_status & STRELA_CMD_PENDING_INT_EXEC) {
		priv->irq_status = 0;
		complete(&priv->exec_done);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int strela_probe(struct platform_device *pdev) {
	struct device *physical_dev = &pdev->dev;
	struct strela_data *priv;
	int minor, ret, irq;

	priv = devm_kzalloc(physical_dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	init_completion(&priv->conf_done);
	init_completion(&priv->exec_done);

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

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(physical_dev, "Unable to get IRQ\n");
		return irq;
	}

	ret = devm_request_threaded_irq(
		physical_dev, irq, strela_irq_handler, strela_irq_thread_fn,
		IRQF_ONESHOT | IRQF_SHARED, dev_name(physical_dev), priv
	);
	if (ret < 0) {
		dev_err(physical_dev, "Failed to request IRQ %d\n", irq);
		return ret;
	}

	// grep strela /proc/devices see dynamically allocated major number
	minor = ida_alloc_max(&strela_ida, STRELA_MAX_NUM - 1, GFP_KERNEL);
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

#if 0
	if (!IS_ENABLED(CONFIG_XILINX_AFI_FPGA)) {
		pr_warn("The Xilinx AFI bridge driver has not been compiled with the kernel "
			"you may need to manually configure some AXI FIFO Interface registers.");
	}
#endif

	if (!IS_ENABLED(CONFIG_FPGA_MGR_ZYNQ_AFI_FPGA)) {
		pr_warn("The Pynq AFI bridge driver has not been compiled with the kernel "
			"you may need to manually configure some AXI FIFO Interface registers.");
	}

	ret = alloc_chrdev_region(&strela_base_dev_num, 0, STRELA_MAX_NUM, "strela");
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
	unregister_chrdev_region(strela_base_dev_num, STRELA_MAX_NUM);
	return ret;
}

static void __exit
strela_driver_exit(void) {
	platform_driver_unregister(&dev_driver);
	class_destroy(strela_class);
	unregister_chrdev_region(strela_base_dev_num, STRELA_MAX_NUM);
	pr_info("STRELA: Global driver removed\n");
}

module_init(strela_driver_init);
module_exit(strela_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Bellani");
MODULE_DESCRIPTION("STRELA platform driver.");
