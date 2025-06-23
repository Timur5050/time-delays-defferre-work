#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/timex.h>
#include <linux/time.h>
#include <asm/msr.h>
#include <linux/jiffies.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Timur");
MODULE_DESCRIPTION("A simple char device driver with single device");
MODULE_VERSION("1.0");

struct simplechar_dev {
    char *data;
    unsigned long size;
    unsigned long last_jiffies;
    cycles_t last_cycles;
    unsigned long min_interval_ms;
    bool interval_set; // Додано для відстеження встановлення інтервалу
    struct cdev cdev;
};

static struct simplechar_dev simplechar_device;
static dev_t simplechar_devno;
static struct class *simplechar_class;
#define BUFFER_SIZE 1024

static int simplechar_open(struct inode *inode, struct file *filp)
{
    filp->private_data = &simplechar_device;
    printk(KERN_INFO "simplechar: Opened device, major=%d, minor=%d\n",
           MAJOR(inode->i_rdev), MINOR(inode->i_rdev));
    return 0;
}

static int simplechar_release(struct inode *inode, struct file *filp)
{
    printk(KERN_INFO "simplechar: Released device, major=%d, minor=%d\n",
           MAJOR(inode->i_rdev), MINOR(inode->i_rdev));
    return 0;
}

static ssize_t simplechar_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct simplechar_dev *dev = filp->private_data;
    unsigned long curr_jiffies = jiffies;
    cycles_t curr_cycles;
    unsigned long jiffies_diff_ms;
    struct timespec64 tv, ts;
    char tmp_buf[BUFFER_SIZE];
    int len;
    ssize_t retval = 0;
    printk(KERN_INFO "simplechar: 1\n");

    if (dev->size == 0) {
        printk(KERN_INFO "simplechar: no data\n");
        return 0;
    }

    if (*f_pos + count > dev->size)
        count = dev->size - *f_pos;
    printk(KERN_INFO "simplechar: 2\n");

    preempt_disable();
    curr_cycles = get_cycles();
    preempt_enable();
    printk(KERN_INFO "simplechar: 3\n");

    jiffies_diff_ms = jiffies_to_msecs((long)curr_jiffies - (long)dev->last_jiffies);
    // Перевіряємо інтервал лише якщо він встановлений
    if (dev->interval_set && time_before(curr_jiffies, dev->last_jiffies + msecs_to_jiffies(dev->min_interval_ms))) {
        printk(KERN_INFO "simplechar: Read too soon, interval %lu ms not elapsed\n", dev->min_interval_ms);
        return -EAGAIN;
    }
    printk(KERN_INFO "simplechar: 4\n");

    ktime_get_ts64(&tv);
    ktime_get_real_ts64(&ts);
    printk(KERN_INFO "simplechar: 5\n");

    len = snprintf(tmp_buf, BUFFER_SIZE,
                   "jiffies: %lu\n"
                   "jiffies_diff_ms: %lu\n"
                   "cycles_diff: %llu\n"
                   "timeval: %ld.%09ld\n"
                   "timespec: %ld.%09ld\n"
                   "data: %.*s\n",
                   curr_jiffies, jiffies_diff_ms,
                   (unsigned long long)(curr_cycles - dev->last_cycles),
                   tv.tv_sec, tv.tv_nsec,
                   ts.tv_sec, ts.tv_nsec,
                   (int)count, dev->data + *f_pos);

    if (copy_to_user(buf, tmp_buf, len)) {
        printk(KERN_ERR "simplechar: Failed to copy data to user\n");
        return -EFAULT;
    }

    dev->last_jiffies = curr_jiffies;
    dev->last_cycles = curr_cycles;
    dev->interval_set = true; // Позначаємо, що інтервал тепер активний
    *f_pos += len;
    retval = len;
    printk(KERN_INFO "simplechar: Read %d bytes from pos %lld\n", len, *f_pos);
    return retval;
}

static ssize_t simplechar_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct simplechar_dev *dev = filp->private_data;
    char tmp_buf[BUFFER_SIZE];
    unsigned long new_interval;
    ssize_t retval = 0;

    if (*f_pos + count > BUFFER_SIZE) {
        count = BUFFER_SIZE - *f_pos;
        if (count == 0) {
            printk(KERN_ERR "simplechar: Buffer full\n");
            return -ENOSPC;
        }
    }

    if (copy_from_user(tmp_buf, buf, count)) {
        printk(KERN_ERR "simplechar: Failed to copy data from user\n");
        return -EFAULT;
    }
    tmp_buf[count] = '\0';

    if (strncmp(tmp_buf, "reset", 5) == 0) {
        dev->last_jiffies = jiffies - msecs_to_jiffies(dev->min_interval_ms) - 1; // Дозволяємо зчитування після reset
        preempt_disable();
        dev->last_cycles = get_cycles();
        preempt_enable();
        printk(KERN_INFO "simplechar: Reset jiffies and cycles\n");
        return count;
    }

    if (sscanf(tmp_buf, "interval=%lu", &new_interval) == 1) {
        dev->min_interval_ms = new_interval;
        dev->last_jiffies = jiffies; // Ініціалізуємо last_jiffies при встановленні інтервалу
        dev->interval_set = true; // Позначаємо, що інтервал встановлено
        printk(KERN_INFO "simplechar: Set interval to %lu ms\n", new_interval);
        return count;
    }

    memcpy(dev->data + *f_pos, tmp_buf, count);
    *f_pos += count;
    if (dev->size < *f_pos)
        dev->size = *f_pos;
    retval = count;
    printk(KERN_INFO "simplechar: Wrote %zd bytes to pos %lld\n", count, *f_pos);
    return retval;
}

static loff_t simplechar_llseek(struct file *filp, loff_t off, int whence)
{
    struct simplechar_dev *dev = filp->private_data;
    loff_t newpos;
    switch (whence) {
    case 0: // SEEK_SET
        newpos = off;
        break;
    case 1: // SEEK_CUR
        newpos = filp->f_pos + off;
        break;
    case 2: // SEEK_END
        newpos = dev->size + off;
        break;
    default:
        return -EINVAL;
    }
    if (newpos < 0)
        return -EINVAL;
    filp->f_pos = newpos;
    return newpos;
}

static struct file_operations simplechar_fops = {
    .owner = THIS_MODULE,
    .open = simplechar_open,
    .release = simplechar_release,
    .read = simplechar_read,
    .write = simplechar_write,
    .llseek = simplechar_llseek
};

static int __init simplechar_init(void)
{
    int err;

    printk(KERN_INFO "simplechar: Initializing module\n");

    err = alloc_chrdev_region(&simplechar_devno, 0, 1, "simplechartest");
    if (err < 0) {
        printk(KERN_ERR "simplechar: Failed to allocate device number\n");
        return err;
    }

    simplechar_device.data = kmalloc(BUFFER_SIZE, GFP_KERNEL);
    if (!simplechar_device.data) {
        printk(KERN_ERR "simplechar: Failed to allocate buffer\n");
        err = -ENOMEM;
        goto fail_alloc;
    }
    simplechar_device.size = 0;
    simplechar_device.min_interval_ms = 0;
    simplechar_device.interval_set = false; // false
    memset(simplechar_device.data, 0, BUFFER_SIZE);

    cdev_init(&simplechar_device.cdev, &simplechar_fops);
    simplechar_device.cdev.owner = THIS_MODULE;
    err = cdev_add(&simplechar_device.cdev, simplechar_devno, 1);
    if (err) {
        printk(KERN_ERR "simplechar: Failed to add cdev\n");
        goto fail_cdev;
    }

    simplechar_class = class_create("simplechartest");
    if (IS_ERR(simplechar_class)) {
        err = PTR_ERR(simplechar_class);
        printk(KERN_ERR "simplechar: Failed to create class\n");
        goto fail_class;
    }
    device_create(simplechar_class, NULL, simplechar_devno, NULL, "simplechartest");

    printk(KERN_INFO "simplechar: Module initialized successfully\n");
    return 0;

fail_class:
    cdev_del(&simplechar_device.cdev);
fail_cdev:
    kfree(simplechar_device.data);
fail_alloc:
    unregister_chrdev_region(simplechar_devno, 1);
    return err;
}

static void __exit simplechar_exit(void)
{
    device_destroy(simplechar_class, simplechar_devno);
    class_destroy(simplechar_class);
    cdev_del(&simplechar_device.cdev);
    kfree(simplechar_device.data);
    unregister_chrdev_region(simplechar_devno, 1);
    printk(KERN_INFO "simplechar: Module unloaded\n");
}

module_init(simplechar_init);
module_exit(simplechar_exit);