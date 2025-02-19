/*
 * drivers/char/kvm_ivshmem.c - driver for KVM Inter-VM shared memory PCI device
 *
 * Copyright 2009 Cam Macdonell <cam@cs.ualberta.ca>
 *
 * Based on cirrusfb.c and 8139cp.c:
 *         Copyright 1999-2001 Jeff Garzik
 *         Copyright 2001-2004 Jeff Garzik
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>

#define TRUE 1
#define FALSE 0
#define KVM_IVSHMEM_DEVICE_MINOR_NUM 0

enum {
	/* KVM Inter-VM shared memory device register offsets */
	IntrMask        = 0x00,    /* Interrupt Mask */
	IntrStatus      = 0x04,    /* Interrupt Status */
	IVPosition      = 0x08,    /* VM ID */
	Doorbell        = 0x0c,    /* Doorbell */
	UMA_COMM        = 0x10,    /* used for UMA: User Mode Agent */
};

typedef struct kvm_ivshmem_device {
	void __iomem * regs;

	void * base_addr;

	unsigned int regaddr;
	unsigned int reg_size;

	unsigned int ioaddr;
	unsigned int ioaddr_size;
	unsigned int irq;

	struct pci_dev *dev;
	char (*msix_names)[256];
	struct msix_entry *msix_entries;
	int nvectors;

	bool		 enabled;
} kvm_ivshmem_device;

static kvm_ivshmem_device kvm_ivshmem_dev;
static spinlock_t mmap_lock= __SPIN_LOCK_UNLOCKED();
static int device_major_nr;

static long kvm_ivshmem_ioctl(struct file *, unsigned int, unsigned long);
static int kvm_ivshmem_open(struct inode *, struct file *);
static int kvm_ivshmem_mmap(struct file *filp, struct vm_area_struct * vma);
static int kvm_ivshmem_release(struct inode *, struct file *);

static const struct file_operations kvm_ivshmem_ops = {
	.owner		= THIS_MODULE,
	.open		= kvm_ivshmem_open,
	.unlocked_ioctl = kvm_ivshmem_ioctl,
	.mmap 		= kvm_ivshmem_mmap,
	.release	= kvm_ivshmem_release,
};

static struct pci_device_id kvm_ivshmem_id_table[] = {
	{ 0x1af4, 0x1110, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0 },
};
MODULE_DEVICE_TABLE (pci, kvm_ivshmem_id_table);

static void kvm_ivshmem_remove_device(struct pci_dev* pdev);
static int kvm_ivshmem_probe_device (struct pci_dev *pdev,
				     const struct pci_device_id * ent);

static struct pci_driver kvm_ivshmem_pci_driver = {
	.name		= "kvm-ivshmem",
	.id_table	= kvm_ivshmem_id_table,
	.probe		= kvm_ivshmem_probe_device,
	.remove		= kvm_ivshmem_remove_device,
};

unsigned char *ivshmem_bar2_map_base(void) {
	return kvm_ivshmem_dev.base_addr;
}
EXPORT_SYMBOL(ivshmem_bar2_map_base);


#define IVSHMEM_IOCTL_COMM _IOR('K', 0, int)


static void reset_bitmap(void) {
	unsigned char *bitmap = kvm_ivshmem_dev.base_addr;
	unsigned int  size = kvm_ivshmem_dev.ioaddr_size;

	if (!bitmap) {
		printk("KVM_IVSHMEM: device not mapped\n");
		return;
	}

	memset(bitmap, 0, size);		
}

static void write_test_bitmap_pattern(void) {
	unsigned char *bitmap = kvm_ivshmem_dev.base_addr;
	unsigned int  size = kvm_ivshmem_dev.ioaddr_size;
	int i;

	if (!bitmap) {
		printk("KVM_IVSHMEM: device not mapped\n");
		return;
	}

	for (i = 0; i < size; i ++) {
		bitmap[i] = 'A' + (i%4);
	}
}

static void handle_ivshmen_cmd(int arg) {

	switch (arg) {

	case 1:
		// for testing
		write_test_bitmap_pattern();
		break;

	case 2:
		printk("KVM_IVSHMEM: reset bitmap\n");
		reset_bitmap();
		break;

	case 3:
		// TODO
		break;

	case 0x50:
	case 0x51:
	case 0x52:

		// this is for stopping the test
		printk("KVM_IVSHMEM: writing to COMM reg with value=%x\n", arg);
		writel(arg, kvm_ivshmem_dev.regs + UMA_COMM);
		break;

	default:
		break;
		
	}
}

static long kvm_ivshmem_ioctl(struct file * filp,
			     unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case IVSHMEM_IOCTL_COMM:

		handle_ivshmen_cmd(arg);

		printk("KVM_IVSHMEM: bar2_map_base:%lx\n", ivshmem_bar2_map_base());
		break;
	default:
		printk("KVM_IVSHMEM: bad ioctl\n");
	}

	return 0;
}

static int kvm_ivshmem_open(struct inode * inode, struct file * filp)
{
   printk(KERN_INFO "Opening kvm_ivshmem device\n");

   if (MINOR(inode->i_rdev) != KVM_IVSHMEM_DEVICE_MINOR_NUM) {
	  printk(KERN_INFO "minor number is %d\n", KVM_IVSHMEM_DEVICE_MINOR_NUM);
	  return -ENODEV;
   }

   return 0;
}


#ifndef VM_RESERVED
#define VM_RESERVED (VM_DONTEXPAND | VM_DONTDUMP)
#endif

static int kvm_ivshmem_mmap(struct file *filp, struct vm_area_struct * vma)
{
	unsigned long len;
	unsigned long off;
	unsigned long start;


	spin_lock(&mmap_lock);

	off = vma->vm_pgoff << PAGE_SHIFT;
	start = kvm_ivshmem_dev.ioaddr;

	len=PAGE_ALIGN((start & ~PAGE_MASK) + kvm_ivshmem_dev.ioaddr_size);
	start &= PAGE_MASK;

	printk(KERN_INFO "%lu - %lu + %lu\n",vma->vm_end ,vma->vm_start, off);
	printk(KERN_INFO "%lu > %lu\n",(vma->vm_end - vma->vm_start + off), len);

	if ((vma->vm_end - vma->vm_start + off) > len) {
		spin_unlock(&mmap_lock);
		return -EINVAL;
	}

	off += start;
	vma->vm_pgoff = off >> PAGE_SHIFT;

	vma->vm_flags |= VM_SHARED|VM_RESERVED;

	if(io_remap_pfn_range(vma, vma->vm_start,
			      off >> PAGE_SHIFT, vma->vm_end - vma->vm_start,
			      vma->vm_page_prot))
	{
		printk("mmap failed\n");
		spin_unlock(&mmap_lock);
		return -ENXIO;
	}

	spin_unlock(&mmap_lock);
	return 0;
}

static int kvm_ivshmem_release(struct inode * inode, struct file * filp)
{
   return 0;
}


static int kvm_ivshmem_probe_device (struct pci_dev *pdev,
				     const struct pci_device_id * ent) {

	int result;

	printk("KVM_IVSHMEM: Probing for KVM_IVSHMEM Device\n");

	result = pci_enable_device(pdev);
	if (result) {
		printk(KERN_ERR "Cannot probe KVM_IVSHMEM device %s: error %d\n",
		pci_name(pdev), result);
		return result;
	}

	result = pci_request_regions(pdev, "kvm_ivshmem");
	if (result < 0) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot request regions\n");
		goto pci_disable;
	} else {
		printk(KERN_ERR "KVM_IVSHMEM: result is %d\n", result);
	}

	kvm_ivshmem_dev.ioaddr = pci_resource_start(pdev, 2);
	kvm_ivshmem_dev.ioaddr_size = pci_resource_len(pdev, 2);
	kvm_ivshmem_dev.base_addr = pci_iomap(pdev, 2, 0);

	if (!kvm_ivshmem_dev.base_addr) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot iomap region of size %d\n",
							kvm_ivshmem_dev.ioaddr_size);
		goto pci_release;
	}

	printk(KERN_INFO "KVM_IVSHMEM: ioaddr = %x, base_addr = %lx, ioaddr_size = %d\n",
	       kvm_ivshmem_dev.ioaddr,
	       kvm_ivshmem_dev.base_addr,
	       kvm_ivshmem_dev.ioaddr_size);

	kvm_ivshmem_dev.regaddr =  pci_resource_start(pdev, 0);
	kvm_ivshmem_dev.reg_size = pci_resource_len(pdev, 0);
	kvm_ivshmem_dev.regs = pci_iomap(pdev, 0, 0x100);

	kvm_ivshmem_dev.dev = pdev;

	if (!kvm_ivshmem_dev.regs) {
		printk(KERN_ERR "KVM_IVSHMEM: cannot ioremap registers of size %d\n",
							kvm_ivshmem_dev.reg_size);
		goto reg_release;
	}

	/* set all masks to off, we do not use them */
	writel(0x0, kvm_ivshmem_dev.regs + IntrMask);

	kvm_ivshmem_dev.enabled = true;

	return 0;

reg_release:
	pci_iounmap(pdev, kvm_ivshmem_dev.base_addr);
pci_release:
	pci_release_regions(pdev);
pci_disable:
	pci_disable_device(pdev);
	return -EBUSY;
}

static void kvm_ivshmem_remove_device(struct pci_dev* pdev)
{

	printk(KERN_INFO "Unregister kvm_ivshmem device.\n");
	free_irq(pdev->irq,&kvm_ivshmem_dev);
	pci_iounmap(pdev, kvm_ivshmem_dev.regs);
	pci_iounmap(pdev, kvm_ivshmem_dev.base_addr);
	pci_release_regions(pdev);
	pci_disable_device(pdev);

}

static void __exit kvm_ivshmem_cleanup_module (void)
{
	pci_unregister_driver (&kvm_ivshmem_pci_driver);
	unregister_chrdev(device_major_nr, "kvm_ivshmem");
}

static int __init kvm_ivshmem_init_module (void)
{

	int err = -ENOMEM;
	printk("KVM_IVSHMEM init\n");

	/* Register device node ops. */
	err = register_chrdev(0, "kvm_ivshmem", &kvm_ivshmem_ops);
	if (err < 0) {
		printk(KERN_ERR "Unable to register kvm_ivshmem device\n");
		return err;
	}

	device_major_nr = err;
	printk("KVM_IVSHMEM: Major device number is: %d\n", device_major_nr);

	err = pci_register_driver(&kvm_ivshmem_pci_driver);
	if (err < 0) {
		goto error;
	}

	return 0;

error:
	unregister_chrdev(device_major_nr, "kvm_ivshmem");
	return err;
}



module_init(kvm_ivshmem_init_module);
module_exit(kvm_ivshmem_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cam Macdonell <cam@cs.ualberta.ca>");
MODULE_DESCRIPTION("KVM inter-VM shared memory module");
MODULE_VERSION("1.0");
