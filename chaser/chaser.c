/*
 * Author : Thomas Stäheli
*/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <asm/io.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/mutex.h>

#define LED_ADDR 0xFF200000 
#define NUM_LEDS 10
#define MAX_SEQUENCES 16
#define DEFAULT_INTERVAL 1000 // 1 seconde

enum direction { UP, DOWN };

// Special structure for the timer, so he can handle the sequence
struct sequence_info {
    uint16_t led_value;
    enum direction dir;
    uint8_t finish_flag;
};

// Private structure of the driver
struct priv {
    struct device *device;
    dev_t dev;
    struct cdev cdev;
    void __iomem *led_base;
    struct class *cls;
    struct task_struct *kthread;
    struct kfifo sequence_fifo;
    wait_queue_head_t wq;
    struct timer_list timer;
    struct sequence_info sequence_info;
    unsigned int interval;
    spinlock_t interval_lock;
    spinlock_t seq_lock;
    atomic_t completed_sequences;
    struct mutex fifo_lock;
};

/* 
 * chaser_timer - Timer callback for LED chasing effect
 * @timer: Pointer to the triggering timer_list structure
 *
 * Updates the LED values, rearms the timer if needed, and handles sequence 
 * completion. Uses spin_lock_irqsave for synchronization. 
 * Accesses hardware registers via iowrite32.
 */
static void chaser_timer(struct timer_list *timer) 
{
    struct priv *priv = from_timer(priv, timer, timer);
    unsigned long flags;

    spin_lock_irqsave(&priv->seq_lock, flags);
    iowrite32(priv->sequence_info.led_value, priv->led_base);

    // Check if the timer need to stop rearming him-self or not
    if(priv->sequence_info.led_value > 0x00 && priv->sequence_info.led_value <= (1 << (NUM_LEDS - 1))) 
    {
        unsigned int interval;
        spin_lock(&priv->interval_lock);
        interval = priv->interval;
        spin_unlock(&priv->interval_lock);
        // Rearming to default interval time
        mod_timer(&priv->timer, jiffies + msecs_to_jiffies(priv->interval));
        // Shift the led_value depending on the sequence
        priv->sequence_info.led_value = priv->sequence_info.dir == UP ?
        priv->sequence_info.led_value << 1 :
        priv->sequence_info.led_value >> 1;

        pr_info("dir = %d : val = %u", priv->sequence_info.dir, priv->sequence_info.led_value);
    } else {
        // End of sequence
        priv->sequence_info.finish_flag = 1;
        atomic_inc(&priv->completed_sequences);
        wake_up_interruptible(&priv->wq);
    }
    
    spin_unlock_irqrestore(&priv->seq_lock, flags);
}

/* 
 * chaser_thread - Kernel thread handling LED sequence commands
 * @data: Pointer to priv structure (driver's private data)
 *
 * Reads commands from a kfifo in a loop, configures LED sequences, 
 * and controls the timer. Uses wait_event_interruptible and mutex_lock.
 * Returns 0 on thread stop (via kthread_should_stop).
 */
static int chaser_thread(void *data)
{
    struct priv* priv = (struct priv *) data;
    enum direction dir;
    unsigned int bytes_read;
    unsigned long flags;

    // request_stop()
    while (!kthread_should_stop()) {
        // Wait until the fifo have a cmd
        wait_event_interruptible(priv->wq, kthread_should_stop() || !kfifo_is_empty(&priv->sequence_fifo));
        // Check if the task should stop
        if (kthread_should_stop())
            break;
        // Get the cmd
        while (true) {
            mutex_lock(&priv->fifo_lock);
            bytes_read = kfifo_out(&priv->sequence_fifo, &dir, sizeof(dir));
            mutex_unlock(&priv->fifo_lock);

            if (bytes_read != sizeof(dir)) {
                break;
            }

            spin_lock_irqsave(&priv->seq_lock, flags);
            // Setuping timer args
            if (dir == UP) {
                // Up sequence
                priv->sequence_info.dir = UP;
                priv->sequence_info.led_value = 1;
            } else {
                // Down sequence
                priv->sequence_info.dir = DOWN;
                priv->sequence_info.led_value = 1 << (NUM_LEDS - 1);
            }

            // Setup de finish flag to 0
            priv->sequence_info.finish_flag = 0;
            spin_unlock_irqrestore(&priv->seq_lock, flags);
            // Arming the timer to 0 s, to start directly
            mod_timer(&priv->timer, jiffies + msecs_to_jiffies(0));
            wait_event_interruptible(priv->wq, priv->sequence_info.finish_flag);
        }
    }
    
    return 0;
}

/* 
 * chaser_open - Device open callback
 * @inode: Pointer to file's inode structure
 * @file:  Pointer to associated file structure
 *
 * Initializes file->private_data with driver data using container_of.
 * Always returns 0 (success).
 */
static int chaser_open(struct inode *inode, struct file *file)
{
    // Setup the private_data into the file, so we can get private data in chaser_write
    struct priv *priv = container_of(inode->i_cdev, struct priv, cdev);
    file->private_data = priv;
    return 0;
}

/* 
 * chaser_write - Userspace write callback
 * @file:  Pointer to file structure
 * @buf:   User-space buffer containing commands ("up" or "down")
 * @count: Size of data to write
 * @ppos:  File position offset (ignored)
 *
 * Copies data using copy_from_user, validates commands, 
 * and pushes them to kfifo. Returns number of bytes written 
 * or error code (e.g., -EINVAL/-ENOSPC).
 */
static ssize_t chaser_write(struct file *file, const char __user *buf, 
                           size_t count, loff_t *ppos)
{
    // Get the private_data from the setup that chaser_open done
    struct priv *priv = (struct priv *)file->private_data;
    char cmd[16];
    enum direction dir;
    int ret;

    if (count > 15)
        return -EINVAL;

    if (copy_from_user(cmd, buf, count))
        return -EFAULT;
    // Force end of the stream
    cmd[count] = '\0';
    // Check if cmd is correct
    if (strcmp(cmd, "up\n") == 0) {
        dir = UP;
    } else if (strcmp(cmd, "down\n") == 0) {
        dir = DOWN;
    } else {
        pr_err("Commande invalide: %s\n", cmd);
        return -EINVAL;
    }

    // Check if the buffer is not full
    mutex_lock(&priv->fifo_lock);
    ret = kfifo_in(&priv->sequence_fifo, &dir, sizeof(dir));
    mutex_unlock(&priv->fifo_lock);
    if (ret != sizeof(dir)) {
        pr_err("File d'attente pleine!\n");
        return -ENOSPC;
    }
    // Wakes up the kthread
    wake_up_interruptible(&priv->wq);
    ret = count;

    return ret;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = chaser_open,
    .write = chaser_write,
};

// Show the interval value in : /sys/devices/platform/soc/ff200000.drv2025/interval 
static ssize_t interval_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct priv *priv = dev_get_drvdata(dev);
    unsigned int interval;
    unsigned long flags;

    // Get the current interval value
    spin_lock_irqsave(&priv->interval_lock, flags);
    interval = priv->interval;
    spin_unlock_irqrestore(&priv->interval_lock, flags);

    return sysfs_emit(buf, "%u\n", interval);
}

// Store the interval value in : /sys/devices/platform/soc/ff200000.drv2025/interval
static ssize_t interval_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count)
{
    struct priv *priv = dev_get_drvdata(dev);
    unsigned int new_interval;
    int ret;
    unsigned long flags;

    // Reading the value from the user
    ret = kstrtouint(buf, 0, &new_interval);
    if (ret)
        return ret;

    if (new_interval == 0)
        return -EINVAL;

    // Assign the new interval
    spin_lock_irqsave(&priv->interval_lock, flags);
    priv->interval = new_interval;
    spin_unlock_irqrestore(&priv->interval_lock, flags);

    return count;
}
static DEVICE_ATTR_RW(interval);

// Get the current light on LED in : /sys/devices/platform/soc/ff200000.drv2025/current_led 
static ssize_t current_led_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct priv *priv = dev_get_drvdata(dev);
    unsigned long flags;
    int led = -1;

    // Reading led_value
    spin_lock_irqsave(&priv->seq_lock, flags);
    if (priv->sequence_info.finish_flag == 0) {
        // Find the first bit set (ffs) and return the index of it
        led = ffs(priv->sequence_info.led_value) - 1;
        if (led < 0 || led >= NUM_LEDS)
            led = -1;
    }
    spin_unlock_irqrestore(&priv->seq_lock, flags);

    return sysfs_emit(buf, "%d\n", led);
}
static DEVICE_ATTR_RO(current_led);

// Get the number of completed sequences in : /sys/devices/platform/soc/ff200000.drv2025/completed_sequences
static ssize_t completed_sequences_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct priv *priv = dev_get_drvdata(dev);
    // Simple read on completed_sequences
    int count = atomic_read(&priv->completed_sequences);

    return sysfs_emit(buf, "%d\n", count);
}
static DEVICE_ATTR_RO(completed_sequences);

// Get the number of cmd waiting in the kfifo in /sys/devices/platform/soc/ff200000.drv2025/queued_sequences
static ssize_t queued_sequences_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct priv *priv = dev_get_drvdata(dev);
    int num;
    // Get the kfifo len = number of cmd waiting
    mutex_lock(&priv->fifo_lock);
    num = kfifo_len(&priv->sequence_fifo) / sizeof(enum direction);
    mutex_unlock(&priv->fifo_lock);

    return sysfs_emit(buf, "%d\n", num);
}
static DEVICE_ATTR_RO(queued_sequences);

// Get the list of cmd waiting in the kfifo in : /sys/devices/platform/soc/ff200000.drv2025/sequence 
static ssize_t sequence_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct priv *priv = dev_get_drvdata(dev);
    enum direction entries[MAX_SEQUENCES];
    int num, i, bytes_read, bytes_written;
    ssize_t total = 0;

    mutex_lock(&priv->fifo_lock);
    // Get the number of cmd in the kfifo
    num = kfifo_len(&priv->sequence_fifo) / sizeof(enum direction);
    if (num > MAX_SEQUENCES)
        num = MAX_SEQUENCES;
    if (num > 0) {
        // Get all the sequence
        bytes_read = kfifo_out(&priv->sequence_fifo, entries, num * sizeof(enum direction));
        // Put them back in the kfifo
        bytes_written = kfifo_in(&priv->sequence_fifo, entries, num * sizeof(enum direction));
        // Doing this check to avoid warnings
        // Check if the written bytes numbers are the same as the read bytes numbers
        if (bytes_written != bytes_read) {
            mutex_unlock(&priv->fifo_lock);
            return -EIO;
        }
    }
    mutex_unlock(&priv->fifo_lock);

    // Listing all the cmd to the user
    for (i = 0; i < num; i++) {
        if (entries[i] == UP)
            total += sysfs_emit_at(buf, total, "up\n");
        else
            total += sysfs_emit_at(buf, total, "down\n");
    }

    return total;
}
static DEVICE_ATTR_RO(sequence);

static int chaser_probe(struct platform_device *pdev)
{
	struct device *clsdev;
	struct priv *priv;
	struct resource *res;
    int err;

	// Allocate memory for our private struct
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (priv == NULL) {
		return -ENOMEM;
	}

	// Store a pointer to our private struct in the platform device
	platform_set_drvdata(pdev, priv);
	// And link our private struct to the struct device
	priv->device = &pdev->dev;

	// Retrieves the physical address of peripherals in the DT
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(priv->device, "failed to get switch memory resource\n");
		return -ENXIO;
	}

	// Map the physical address to a virtual address
	priv->led_base = devm_ioremap_resource(priv->device, res);
	if (IS_ERR(priv->led_base)) {
		return PTR_ERR(priv->led_base);
	}

    // Initialisation du KFIFO
    if (kfifo_alloc(&priv->sequence_fifo, MAX_SEQUENCES * sizeof(enum direction), GFP_KERNEL))
        return -ENOMEM;

    // Init de la waitqueue
    init_waitqueue_head(&priv->wq);
    // Init Spin lock
    spin_lock_init(&priv->interval_lock);
    spin_lock_init(&priv->seq_lock);
    // Init Mutex
    mutex_init(&priv->fifo_lock);
    // Init variable atomic
    atomic_set(&priv->completed_sequences, 0);
    priv->interval = DEFAULT_INTERVAL;

    // Allocation for character driver
    if (alloc_chrdev_region(&priv->dev, 0, 1, "chaser")) {
        pr_err("Chaser : Error Allocation device\n");
		return -ENODEV;
    }

    // Init the device
    cdev_init(&priv->cdev, &fops);
    if (cdev_add(&priv->cdev, priv->dev, 1)) {
        pr_err("Chaser : Error Init cdev\n");
		err = -ENODEV;
        goto err_cdev_add;
    }

    // Create class for chaser
    priv->cls = class_create(THIS_MODULE, "chaser");
	if (IS_ERR(priv->cls)) {
		err = PTR_ERR(priv->cls);
		pr_err("Chaser : Error creating class (%d)\n", err);
		goto err_class_create;
	}

    // Create device 
    clsdev = device_create(priv->cls, NULL, priv->dev, NULL, "chaser");
	if (IS_ERR(clsdev)) {
		err = PTR_ERR(clsdev);
		pr_err("Chaser: Error creating device (%d)\n", err);
		goto err_device_create;
	}

    // Creating all virtual files system
    err = device_create_file(&pdev->dev, &dev_attr_interval);
    if (err)
        goto err_sysfs;
    err = device_create_file(&pdev->dev, &dev_attr_current_led);
    if (err)
        goto err_sysfs;
    err = device_create_file(&pdev->dev, &dev_attr_completed_sequences);
    if (err)
        goto err_sysfs;
    err = device_create_file(&pdev->dev, &dev_attr_queued_sequences);
    if (err)
        goto err_sysfs;
    err = device_create_file(&pdev->dev, &dev_attr_sequence);
    if (err)
        goto err_sysfs;

    // Launch thread
    priv->kthread = kthread_run(chaser_thread, priv, "chaser_kthread");
    if (IS_ERR(priv->kthread)) {
        pr_err("Chaser : Error run kthread\n");
        err = PTR_ERR(priv->kthread);
        goto err_kthread;
    }

    // Initialize the timer
	timer_setup(&priv->timer, chaser_timer, 0);

    // Clear led state
    iowrite32(0, priv->led_base);
	pr_info("Chaser ready!\n");

	return 0;

err_kthread:
    device_remove_file(&pdev->dev, &dev_attr_interval);
    device_remove_file(&pdev->dev, &dev_attr_current_led);
    device_remove_file(&pdev->dev, &dev_attr_completed_sequences);
    device_remove_file(&pdev->dev, &dev_attr_queued_sequences);
    device_remove_file(&pdev->dev, &dev_attr_sequence);
err_sysfs:
    device_destroy(priv->cls, priv->dev);
err_device_create:
	class_destroy(priv->cls);
err_class_create:
	cdev_del(&priv->cdev);
err_cdev_add:
	unregister_chrdev_region(priv->dev, 1);
    return err;
}

static int chaser_remove(struct platform_device *pdev)
{
    struct priv *priv = platform_get_drvdata(pdev);
    // Stop thread and reseting led value
    iowrite32(0, priv->led_base);
    kthread_stop(priv->kthread);
    del_timer_sync(&priv->timer);
    // Removing all virtual files system
    device_remove_file(&pdev->dev, &dev_attr_interval);
    device_remove_file(&pdev->dev, &dev_attr_current_led);
    device_remove_file(&pdev->dev, &dev_attr_completed_sequences);
    device_remove_file(&pdev->dev, &dev_attr_queued_sequences);
    device_remove_file(&pdev->dev, &dev_attr_sequence);
    // Removing character device structure
    device_destroy(priv->cls, priv->dev);
    class_destroy(priv->cls);
    cdev_del(&priv->cdev);
    unregister_chrdev_region(priv->dev, 1);
    kfifo_free(&priv->sequence_fifo);
    pr_info("Chaser removed!");

    return 0;
}

static const struct of_device_id chaser_driver_id[] = {
    { .compatible = "drv2025" },
    { /* END */ },
};

MODULE_DEVICE_TABLE(of, chaser_driver_id);

static struct platform_driver chaser_driver = {
    .driver = {
        .name = "chaser",
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(chaser_driver_id),
    },
    .probe = chaser_probe,
    .remove = chaser_remove,
};

static int __init chaser_init(void) 
{
    return platform_driver_register(&chaser_driver);
}

static void __exit chaser_exit(void) {
    platform_driver_unregister(&chaser_driver);
}

module_init(chaser_init);
module_exit(chaser_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("REDS");
MODULE_DESCRIPTION("Driver de chenillard pour DE1-SoC");