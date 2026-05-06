// Kernel Module for Strela CGRA AXI DMA adapter
// Originally written by Juan Granja on July 2024 and updated by Diego Bellani 2026

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>
 
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/mm.h>

#include <asm/errno.h>
#include <asm/io.h>

#include "cgra_dma.h"
#include "cgra_regs.h"

#define CGRA_BASE_ADDRESS 0x43C00000
#define CGRA_IO_SIZE      0x1000

static uint64_t dma_mask;
static dev_t dev_number;
static struct cdev *cdev_struct_ptr;
static struct class *class_struct_ptr;
static struct device *device_struct_ptr;

static void *addr_vir;
static dma_addr_t addr_bus;
// NOTE: This functions should be noop when using dma_mmap_coherent.
// Xil_DCacheFlushRange
#define FLUSH_D_CACHE() if (0) dma_sync_single_for_device(device_struct_ptr, addr_bus, CGRA_DATA_REGION_SIZE, DMA_TO_DEVICE)
// Xil_DCacheInvalidateRange
#define INVAL_D_CACHE() if (0) dma_sync_single_for_cpu(device_struct_ptr, addr_bus, CGRA_DATA_REGION_SIZE, DMA_FROM_DEVICE)

static void __iomem *cgra_io_base_addr;

static int
device_open(struct inode *inode, struct file *file) {
    if (!try_module_get(THIS_MODULE)) {
        pr_err("[Mod] Failed to lock module.\n");
        return -ENODEV;
    }

    return 0;
}

static int
device_release(struct inode *inode, struct file *file) {
    module_put(THIS_MODULE);
    return 0;
}

static long
device_ioctl(struct file *file, unsigned int ioctl_num, unsigned long ioctl_param) {
    long ret = 0;
 
    switch (ioctl_num) {
        case IOCTL_CGRA_CONTROL: {
            CGRA_control_t cgra_ctrl;

            if (copy_from_user(&cgra_ctrl, (void __user *)ioctl_param, sizeof cgra_ctrl))
                pr_err("[Mod] Copy from user failed\n");

/*
#define iowrite32(b, addr) do {
    printk(KERN_INFO "0x%p <- %x", (addr), (b)); \
    (iowrite32)((b), (addr)); \
} while (0)
*/
            iowrite32(addr_bus + cgra_ctrl.conf_offs,      cgra_io_base_addr + CGRA_REG_CONF_ADDR);
            iowrite32(cgra_ctrl.conf_count*CGRA_WORD_SIZE, cgra_io_base_addr + CGRA_REG_CONF_SIZE);

            iowrite32(addr_bus + cgra_ctrl.in0_offs*CGRA_WORD_SIZE,                      cgra_io_base_addr + CGRA_REG_INP0_ADDR);
            iowrite32(CGRA_PACK_STRIDE_COUNT(cgra_ctrl.in0_stride, cgra_ctrl.in0_count), cgra_io_base_addr + CGRA_REG_INP0_SIZE);
            iowrite32(addr_bus + cgra_ctrl.in1_offs*CGRA_WORD_SIZE,                      cgra_io_base_addr + CGRA_REG_INP1_ADDR);
            iowrite32(CGRA_PACK_STRIDE_COUNT(cgra_ctrl.in1_stride, cgra_ctrl.in1_count), cgra_io_base_addr + CGRA_REG_INP1_SIZE);
            iowrite32(addr_bus + cgra_ctrl.in2_offs*CGRA_WORD_SIZE,                      cgra_io_base_addr + CGRA_REG_INP2_ADDR);
            iowrite32(CGRA_PACK_STRIDE_COUNT(cgra_ctrl.in2_stride, cgra_ctrl.in2_count), cgra_io_base_addr + CGRA_REG_INP2_SIZE);
            iowrite32(addr_bus + cgra_ctrl.in3_offs*CGRA_WORD_SIZE,                      cgra_io_base_addr + CGRA_REG_INP3_ADDR);
            iowrite32(CGRA_PACK_STRIDE_COUNT(cgra_ctrl.in3_stride, cgra_ctrl.in3_count), cgra_io_base_addr + CGRA_REG_INP3_SIZE);

            iowrite32(addr_bus + cgra_ctrl.out0_offs*CGRA_WORD_SIZE,  cgra_io_base_addr + CGRA_REG_OUT0_ADDR);
            iowrite32(           cgra_ctrl.out0_count*CGRA_WORD_SIZE, cgra_io_base_addr + CGRA_REG_OUT0_SIZE);
            iowrite32(addr_bus + cgra_ctrl.out1_offs*CGRA_WORD_SIZE,  cgra_io_base_addr + CGRA_REG_OUT1_ADDR);
            iowrite32(           cgra_ctrl.out1_count*CGRA_WORD_SIZE, cgra_io_base_addr + CGRA_REG_OUT1_SIZE);
            iowrite32(addr_bus + cgra_ctrl.out2_offs*CGRA_WORD_SIZE,  cgra_io_base_addr + CGRA_REG_OUT2_ADDR);
            iowrite32(           cgra_ctrl.out2_count*CGRA_WORD_SIZE, cgra_io_base_addr + CGRA_REG_OUT2_SIZE);
            iowrite32(addr_bus + cgra_ctrl.out3_offs*CGRA_WORD_SIZE,  cgra_io_base_addr + CGRA_REG_OUT3_ADDR);
            iowrite32(           cgra_ctrl.out3_count*CGRA_WORD_SIZE, cgra_io_base_addr + CGRA_REG_OUT3_SIZE);

            // Maybe unnecessary...
            iowrite32(1, cgra_io_base_addr + CGRA_REG_OUT_ARB_HOLD);
// #undef iowrite32
            break;
        }
        case IOCTL_CGRA_CONFIG: {
            FLUSH_D_CACHE();

            iowrite32(CGRA_CMD_CLEAR_STATE,  cgra_io_base_addr + CGRA_REG_CTRL);
            iowrite32(CGRA_CMD_CLEAR_CONFIG, cgra_io_base_addr + CGRA_REG_CTRL);
            iowrite32(1,                     cgra_io_base_addr + CGRA_REG_RESET_DMA);

            iowrite32(CGRA_CMD_LOAD_CONFIG, cgra_io_base_addr + CGRA_REG_CTRL);

            u32 val = 0;
            ret = readl_poll_timeout(
                cgra_io_base_addr + CGRA_REG_CTRL,
                val, (val & CGRA_CMD_DONE_CONFIG), 10, 5000
            );
            break;
        }
        case IOCTL_CGRA_EXEC: {
            FLUSH_D_CACHE();
            iowrite32(CGRA_CMD_START_EXEC, cgra_io_base_addr + CGRA_REG_CTRL);

            u32 val = 0;
            ret = readl_poll_timeout(
                cgra_io_base_addr + CGRA_REG_CTRL,
                val, (val & CGRA_CMD_DONE_EXEC), 10, 500000
            );
            INVAL_D_CACHE();
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
device_mmap(struct file *fp, struct vm_area_struct *vma) {
    int ret = 0;
    unsigned long requested_size = vma->vm_end - vma->vm_start;

    if (requested_size > CGRA_DATA_REGION_SIZE) {
        pr_err("[Mod] MMAP failed: requested %lu bytes, max is %d\n",
               requested_size, CGRA_DATA_REGION_SIZE);
        return -EINVAL;
    }

    ret = dma_mmap_coherent(device_struct_ptr, vma, addr_vir, addr_bus, requested_size);

    if (ret < 0) {
        pr_err("[Mod] dma_mmap_coherent failed with error: %d\n", ret);
        return ret;
    }

    return 0;
}

static struct file_operations fops = {
    .unlocked_ioctl = device_ioctl,
    .mmap           = device_mmap,
    .open           = device_open,
    .release        = device_release,
};

static bool
is_fpga_programmed(void) {
    struct device_node *np;
    struct platform_device *pdev;
    struct fpga_manager *mgr;
    bool programmed = false;

    np = of_find_compatible_node(NULL, NULL, "xlnx,zynq-devcfg-1.0");
    if (!np)
        return false;

    pdev = of_find_device_by_node(np);
    of_node_put(np);
    if (!pdev)
        return false;

    mgr = fpga_mgr_get(&pdev->dev);
    if (IS_ERR(mgr)) {
        put_device(&pdev->dev);
        return false;
    }

    // 4. Check the state
    if (mgr->state == FPGA_MGR_STATE_OPERATING)
        programmed = true;

    fpga_mgr_put(mgr);
    put_device(&pdev->dev);

    return programmed;
}

// This is needed for automatic configuration of HP AXI ports and other things
// from the device overlay.
// IS_ENABLED(CONFIG_XILINX_AFI_FPGA)
static int __init
chardev2_init(void) {
    if (!is_fpga_programmed()) {
        pr_err("Refusing to load: FPGA is not programmed. Avoided AXI lockup!\n");
        return -ENODEV;
    }

    int errorType;
    printk("[Mod] starting insertion\n");

    if (alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME) < 0)
        return -EIO;
    
    cdev_struct_ptr = cdev_alloc();
    if (cdev_struct_ptr == NULL) {
        errorType = EIO;
        goto free_device_number;
    }

    cdev_struct_ptr->owner = THIS_MODULE;
    cdev_struct_ptr->ops = &fops;

    if (cdev_add(cdev_struct_ptr, dev_number, 1)) {
        errorType = EIO;
        goto free_cdev;
    }

    #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
        class_struct_ptr = class_create(DEVICE_NAME);
    #else
        class_struct_ptr = class_create(THIS_MODULE, DEVICE_NAME);
    #endif
    if (IS_ERR(class_struct_ptr)) {
        pr_err("[Mod] no udev support\n");
        errorType = EIO;
        goto free_cdev;
    }

    device_struct_ptr = device_create(class_struct_ptr, NULL, dev_number, NULL, DEVICE_NAME);
    if (IS_ERR(device_struct_ptr)) {
        pr_err("[Mod] device_create failed\n");
        errorType = EIO;
        goto free_class;
    }

    dma_mask = DMA_BIT_MASK(32);
    device_struct_ptr->dma_mask = &dma_mask;

    if (dma_set_mask_and_coherent(device_struct_ptr, DMA_BIT_MASK(32))) {
        pr_err("[Mod] setting mask failed\n");
        errorType = EIO;
        goto free_dev;
    }

    addr_vir = dma_alloc_coherent(device_struct_ptr, CGRA_DATA_REGION_SIZE, &addr_bus, GFP_KERNEL);

    if (addr_vir == NULL) {
        printk("[Mod] virtual address null, dma failed!\n");
        errorType = ENOMEM;
        goto free_dev;
    }

    if (!request_mem_region(CGRA_BASE_ADDRESS, CGRA_IO_SIZE, DEVICE_NAME)) {
        pr_err("[Mod] IO mem request failed\n");
        errorType = EIO;
        goto free_dma;
    }

    cgra_io_base_addr = ioremap(CGRA_BASE_ADDRESS, CGRA_IO_SIZE);

    u32 challenge1 = 1; // get_random_u32();
    u32 challenge2 = 2; // get_random_u32();
    iowrite32(challenge1, cgra_io_base_addr + CGRA_REG_OPA);
    iowrite32(challenge2, cgra_io_base_addr + CGRA_REG_OPB);
    u32 response = ioread32(cgra_io_base_addr + CGRA_REG_OPR);

    if (response != challenge1 + challenge2) {
        pr_err("[Mod] Bounce back register test failed!");
        errorType = EIO;
        goto free_dma;
    }

    printk("[Mod] success\n");

    return 0;

free_dma:
    dma_free_coherent(device_struct_ptr, CGRA_DATA_REGION_SIZE, addr_vir, addr_bus);
free_dev:
    device_destroy(class_struct_ptr, dev_number);
free_class:
    class_destroy(class_struct_ptr);
free_cdev:
    kobject_put(&cdev_struct_ptr->kobj);
free_device_number:
    unregister_chrdev_region(dev_number, 1);
    return -errorType;
}

static void __exit
chardev2_exit(void) {
    iounmap(cgra_io_base_addr);
    release_mem_region(CGRA_BASE_ADDRESS, CGRA_IO_SIZE);
    dma_free_coherent(device_struct_ptr, CGRA_DATA_REGION_SIZE, addr_vir, addr_bus);
    device_destroy(class_struct_ptr, dev_number);
    class_destroy(class_struct_ptr);
    cdev_del(cdev_struct_ptr);
    unregister_chrdev_region(dev_number, 1);
}

module_init(chardev2_init);
module_exit(chardev2_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Bellani");
MODULE_DESCRIPTION("Strela AXI DMA adapter kernel module");
MODULE_VERSION("0.2");
