// Thomas Stäheli
#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/init.h>	/* Needed for the macros */
#include <linux/fs.h>		/* Needed for file_operations */
#include <linux/slab.h>	/* Needed for kmalloc */
#include <linux/uaccess.h>	/* copy_(to|from)_user */
#include <linux/cdev.h>
#include <linux/device.h>

#include <linux/string.h>

#include "accumulate.h"

#define MAJOR_NUM		97
#define DEVICE_NAME		"accumulate"

#define MAX_NB_VALUE		256

static uint64_t accumulate_value;
static int operation = OP_ADD;

static ssize_t accumulate_read(struct file *filp, char __user *buf,
                             size_t count, loff_t *ppos)
{
    uint64_t value;
    int ret;

    if (*ppos >= sizeof(accumulate_value))
        return 0;

    if (count < sizeof(accumulate_value))
        return -EINVAL;

    value = accumulate_value;
    ret = copy_to_user(buf, &value, sizeof(value));
    if (ret)
        return -EFAULT;

    *ppos = sizeof(value);
    return sizeof(value);
}

static ssize_t accumulate_write(struct file *filp, const char __user *buf,
                              size_t count, loff_t *ppos)
{
    uint64_t value;
    int ret;

    if (count != sizeof(value))
        return -EINVAL;

    *ppos = 0;  // Réinitialisation de la position déplacée ici

    ret = copy_from_user(&value, buf, sizeof(value));
    if (ret)
        return -EFAULT;

    switch (operation) {
    case OP_ADD:
        accumulate_value += value;
        break;
    case OP_MULTIPLY:
        accumulate_value *= value;
        break;
    default:
        return -EINVAL;
    }

    return sizeof(value);
}

static long accumulate_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case ACCUMULATE_CMD_RESET:
		accumulate_value = 0;
		break;

	case ACCUMULATE_CMD_CHANGE_OP:
		if (arg != OP_ADD && arg != OP_MULTIPLY) {
			return -EINVAL;
		}
		operation = arg;
		break;

	default:
		return -EINVAL;
	}
	return 0;
}

static int accumulate_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0666);
    return 0;
}

static const struct file_operations accumulate_fops = {
	.owner          = THIS_MODULE,
	.read           = accumulate_read,
	.write          = accumulate_write,
	.unlocked_ioctl = accumulate_ioctl,
};

static struct class *accumulate_class = NULL;

// device data holder, this structure may be extended to hold additional data
struct accumulate_device_data {
    struct cdev cdev;
};

// array of mychar_device_data for
static struct accumulate_device_data accumulate_data;


static int __init accumulate_init(void)
{
	int err;
	dev_t dev;

	err = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);

	accumulate_class = class_create(THIS_MODULE, DEVICE_NAME);
	accumulate_class->dev_uevent = accumulate_uevent;

	// init new device
	cdev_init(&accumulate_data.cdev, &accumulate_fops);
	accumulate_data.cdev.owner = THIS_MODULE;

	// add device to the system where "i" is a Minor number of the new device
	cdev_add(&accumulate_data.cdev, MKDEV(MAJOR_NUM, 0), 1);

	// create device node /dev/mychardev-x where "x" is "i", equal to the Minor number
	device_create(accumulate_class, NULL, MKDEV(MAJOR_NUM, 0), NULL, DEVICE_NAME);

	// register_chrdev(MAJOR_NUM, DEVICE_NAME, &accumulate_fops);

	accumulate_value = 0;

	pr_info("Acumulate ready!\n");
	pr_info("ioctl ACCUMULATE_CMD_RESET: %lu\n", (unsigned long)ACCUMULATE_CMD_RESET);
	pr_info("ioctl ACCUMULATE_CMD_CHANGE_OP: %lu\n", (unsigned long)ACCUMULATE_CMD_CHANGE_OP);

	return 0;
}

static void __exit accumulate_exit(void)
{
	// unregister_chrdev(MAJOR_NUM, DEVICE_NAME);
	device_destroy(accumulate_class, MKDEV(MAJOR_NUM, 0));

	class_unregister(accumulate_class);
    class_destroy(accumulate_class);

    unregister_chrdev_region(MKDEV(MAJOR_NUM, 0), MINORMASK);

	pr_info("Acumulate done!\n");
}

MODULE_AUTHOR("REDS");
MODULE_LICENSE("GPL");

module_init(accumulate_init);
module_exit(accumulate_exit);
