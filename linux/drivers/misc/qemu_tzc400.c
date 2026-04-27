// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test-only QEMU TZC-400 SMC bridge.
 */

#include <linux/fs.h>
#include <linux/arm-smccc.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/io.h>

#define QEMU_TZC400_SMC_CONFIG_REGION	0xc200ff00UL

#define QEMU_TZC400_OK			0
#define QEMU_TZC400_E_DENIED		-1
#define QEMU_TZC400_E_RANGE		-2
#define QEMU_TZC400_E_ALIGN		-3

#define QEMU_TZC400_IOCTL_MAGIC		'z'

struct qemu_tzc400_alloc {
	__u64 phys;
	__u64 size;
};

struct qemu_tzc400_region {
	__u32 filters;
	__u32 region;
	__u64 base;
	__u64 top;
	__u32 sec_attr;
	__u32 nsaid_permissions;
};

struct qemu_tzc400_touch {
	__u32 write;
	__u32 value;
};

#define QEMU_TZC400_IOCTL_ALLOC \
	_IOR(QEMU_TZC400_IOCTL_MAGIC, 0, struct qemu_tzc400_alloc)
#define QEMU_TZC400_IOCTL_CONFIG \
	_IOW(QEMU_TZC400_IOCTL_MAGIC, 1, struct qemu_tzc400_region)
#define QEMU_TZC400_IOCTL_TOUCH \
	_IOWR(QEMU_TZC400_IOCTL_MAGIC, 2, struct qemu_tzc400_touch)

static DEFINE_MUTEX(qemu_tzc400_lock);
static void *qemu_tzc400_page;
static phys_addr_t qemu_tzc400_phys;

static int qemu_tzc400_smc_errno(long ret)
{
	switch (ret) {
	case QEMU_TZC400_OK:
		return 0;
	case QEMU_TZC400_E_DENIED:
		return -EPERM;
	case QEMU_TZC400_E_RANGE:
	case QEMU_TZC400_E_ALIGN:
		return -EINVAL;
	default:
		return -EIO;
	}
}

static int qemu_tzc400_alloc_page(struct qemu_tzc400_alloc __user *argp)
{
	struct qemu_tzc400_alloc alloc;
	int ret = 0;

	mutex_lock(&qemu_tzc400_lock);
	if (!qemu_tzc400_page) {
		qemu_tzc400_page = (void *)get_zeroed_page(GFP_KERNEL);
		if (!qemu_tzc400_page) {
			ret = -ENOMEM;
			goto out_unlock;
		}

		qemu_tzc400_phys = virt_to_phys(qemu_tzc400_page);
	} else {
		memset(qemu_tzc400_page, 0, PAGE_SIZE);
	}

	alloc.phys = qemu_tzc400_phys;
	alloc.size = PAGE_SIZE;

out_unlock:
	mutex_unlock(&qemu_tzc400_lock);

	if (ret)
		return ret;

	if (copy_to_user(argp, &alloc, sizeof(alloc)))
		return -EFAULT;

	return 0;
}

static int qemu_tzc400_config_region(
	const struct qemu_tzc400_region __user *argp)
{
	struct arm_smccc_res res;
	struct qemu_tzc400_region req;
	void *smc_page;
	phys_addr_t smc_phys;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	smc_page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!smc_page)
		return -ENOMEM;

	memcpy(smc_page, &req, sizeof(req));
	smc_phys = virt_to_phys(smc_page);

	arm_smccc_smc(QEMU_TZC400_SMC_CONFIG_REGION, smc_phys, sizeof(req),
		      0, 0, 0, 0, 0, &res);

	ret = qemu_tzc400_smc_errno(res.a0);
	free_page((unsigned long)smc_page);

	return ret;
}

static int qemu_tzc400_touch_page(
	const struct qemu_tzc400_touch __user *argp)
{
	struct qemu_tzc400_touch touch;
	volatile __u32 *page;

	if (copy_from_user(&touch, argp, sizeof(touch)))
		return -EFAULT;

	if (touch.write > 1)
		return -EINVAL;

	mutex_lock(&qemu_tzc400_lock);
	if (!qemu_tzc400_page) {
		mutex_unlock(&qemu_tzc400_lock);
		return -ENODATA;
	}

	page = qemu_tzc400_page;
	if (touch.write)
		WRITE_ONCE(*page, touch.value);
	else
		touch.value = READ_ONCE(*page);

	mutex_unlock(&qemu_tzc400_lock);

	if (copy_to_user((struct qemu_tzc400_touch __user *)argp, &touch,
			 sizeof(touch)))
		return -EFAULT;

	return 0;
}

static long qemu_tzc400_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case QEMU_TZC400_IOCTL_ALLOC:
		return qemu_tzc400_alloc_page(argp);
	case QEMU_TZC400_IOCTL_CONFIG:
		return qemu_tzc400_config_region(argp);
	case QEMU_TZC400_IOCTL_TOUCH:
		return qemu_tzc400_touch_page(argp);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations qemu_tzc400_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = qemu_tzc400_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice qemu_tzc400_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "qemu_tzc400",
	.fops = &qemu_tzc400_fops,
};

static int __init qemu_tzc400_init(void)
{
	return misc_register(&qemu_tzc400_miscdev);
}

static void __exit qemu_tzc400_exit(void)
{
	misc_deregister(&qemu_tzc400_miscdev);

	mutex_lock(&qemu_tzc400_lock);
	if (qemu_tzc400_page) {
		free_page((unsigned long)qemu_tzc400_page);
		qemu_tzc400_page = NULL;
		qemu_tzc400_phys = 0;
	}
	mutex_unlock(&qemu_tzc400_lock);
}

module_init(qemu_tzc400_init);
module_exit(qemu_tzc400_exit);

MODULE_DESCRIPTION("QEMU TZC-400 test-only SMC ioctl bridge");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");
