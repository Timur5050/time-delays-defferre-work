#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Timur");
MODULE_DESCRIPTION("A simple char device driver with single device");
MODULE_VERSION("1.0");

#define BUFFER_SIZE 1024

struct simplechar_dev {
    char *data;
    unsigned long size;
    unsigned long tick_count;
    unsigned long char_count;
    unsigned long work_delay;
    int log_done;
    struct timer_list timer;
    struct tasklet_struct tasklet;
    struct delayed_work work;
    struct workqueue_struct *wq;
    spinlock_t lock;
    struct cdev cdev;
};

static struct simplechar_dev simplechar_device;
static dev_t simplechar_devno;
static struct class *simplechar_class;

static void simplechar_timer_fn(struct timer_list *t);
static void simplechar_tasklet_fn(unsigned long arg);
static void simplechar_work_fn(struct work_struct *work);

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
    char tmp_buf[BUFFER_SIZE];
    unsigned long flags;
    int len;

    spin_lock_irqsave(&dev->lock, flags);

    // Формування повного рядка
    len = snprintf(tmp_buf, BUFFER_SIZE,
                   "data: %s\n"
                   "tick_count: %lu\n"
                   "char_count: %lu\n"
                   "log_done: %d\n",
                   dev->data,
                   dev->tick_count,
                   dev->char_count,
                   dev->log_done);

    spin_unlock_irqrestore(&dev->lock, flags);

    if (*f_pos >= len)
        return 0;

    if (*f_pos + count > len)
        count = len - *f_pos;

    if (copy_to_user(buf, tmp_buf + *f_pos, count)) {
        printk(KERN_ERR "simplechar: Failed to copy data to user\n");
        return -EFAULT;
    }

    *f_pos += count;
    return count;
}


static ssize_t simplechar_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct simplechar_dev *dev = filp->private_data;
    char tmp_buf[BUFFER_SIZE];
    unsigned long new_work_delay;
    unsigned long flags;

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

    spin_lock_irqsave(&dev->lock, flags);

    if (strncmp(tmp_buf, "reset", 5) == 0) {
        dev->size = 0;
        dev->tick_count = 0;
        dev->char_count = 0;
        dev->work_delay = 0;
        dev->log_done = 0;
        memset(dev->data, 0, BUFFER_SIZE);
        mod_timer(&dev->timer, jiffies + msecs_to_jiffies(1000));
        tasklet_kill(&dev->tasklet);
        cancel_delayed_work(&dev->work);
        flush_workqueue(dev->wq);
        spin_unlock_irqrestore(&dev->lock, flags);
        return count;
    }

    if (sscanf(tmp_buf, "work_delay=%lu", &new_work_delay) == 1) {
        dev->work_delay = new_work_delay;
        spin_unlock_irqrestore(&dev->lock, flags);
        return count;
    }

    memcpy(dev->data + *f_pos, tmp_buf, count);
    *f_pos += count;
    if (dev->size < *f_pos)
        dev->size = *f_pos;

    tasklet_schedule(&dev->tasklet);
    queue_delayed_work(dev->wq, &dev->work, msecs_to_jiffies(dev->work_delay));

    spin_unlock_irqrestore(&dev->lock, flags);

    printk(KERN_INFO "simplechar: Wrote %zd bytes to pos %lld\n", count, *f_pos);
    return count;
}

static void simplechar_timer_fn(struct timer_list *t)
{
    struct simplechar_dev *dev = from_timer(dev, t, timer);
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    dev->tick_count++;
    spin_unlock_irqrestore(&dev->lock, flags);

    mod_timer(&dev->timer, jiffies + msecs_to_jiffies(1000));
}

static void simplechar_tasklet_fn(unsigned long arg)
{
    struct simplechar_dev *dev = (struct simplechar_dev *)arg;
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    dev->char_count = 0;
    for (size_t i = 0; i < dev->size; i++) {
        if (dev->data[i] != '\0')
            dev->char_count++;
    }
    spin_unlock_irqrestore(&dev->lock, flags);
}

static void simplechar_work_fn(struct work_struct *work)
{
    struct simplechar_dev *dev = container_of(work, struct simplechar_dev, work.work);
    unsigned long flags;

    msleep(10000);

    spin_lock_irqsave(&dev->lock, flags);
    dev->log_done = 1;
    spin_unlock_irqrestore(&dev->lock, flags);
}

static struct file_operations simplechar_fops = {
    .owner = THIS_MODULE,
    .open = simplechar_open,
    .release = simplechar_release,
    .read = simplechar_read,
    .write = simplechar_write,
};

static int __init simplechar_init(void)
{
    int err;

    printk(KERN_INFO "simplechar: Initializing module\n");

    err = alloc_chrdev_region(&simplechar_devno, 0, 1, "simplechartime");
    if (err < 0) {
        printk(KERN_ERR "simplechar: Failed to allocate device number\n");
        return err;
    }

    simplechar_device.data = kzalloc(BUFFER_SIZE, GFP_KERNEL);
    if (!simplechar_device.data) {
        err = -ENOMEM;
        goto fail_alloc;
    }

    simplechar_device.size = 0;
    simplechar_device.tick_count = 0;
    simplechar_device.char_count = 0;
    simplechar_device.work_delay = 0;
    simplechar_device.log_done = 0;
    spin_lock_init(&simplechar_device.lock);

    timer_setup(&simplechar_device.timer, simplechar_timer_fn, 0);
    mod_timer(&simplechar_device.timer, jiffies + msecs_to_jiffies(1000));

    tasklet_init(&simplechar_device.tasklet, simplechar_tasklet_fn, (unsigned long)&simplechar_device);

    simplechar_device.wq = create_singlethread_workqueue("simplechar_wq");
    if (!simplechar_device.wq) {
        err = -ENOMEM;
        goto fail_wq;
    }

    INIT_DELAYED_WORK(&simplechar_device.work, simplechar_work_fn);

    cdev_init(&simplechar_device.cdev, &simplechar_fops);
    simplechar_device.cdev.owner = THIS_MODULE;
    err = cdev_add(&simplechar_device.cdev, simplechar_devno, 1);
    if (err) {
        printk(KERN_ERR "simplechar: Failed to add cdev\n");
        goto fail_cdev;
    }

    simplechar_class = class_create("simplechartime");
    if (IS_ERR(simplechar_class)) {
        err = PTR_ERR(simplechar_class);
        printk(KERN_ERR "simplechar: Failed to create class\n");
        goto fail_class;
    }

    device_create(simplechar_class, NULL, simplechar_devno, NULL, "simplechartime");

    return 0;

fail_class:
    cdev_del(&simplechar_device.cdev);
fail_cdev:
    destroy_workqueue(simplechar_device.wq);
fail_wq:
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
    flush_workqueue(simplechar_device.wq);
    destroy_workqueue(simplechar_device.wq);
    del_timer_sync(&simplechar_device.timer);
    tasklet_kill(&simplechar_device.tasklet);
    kfree(simplechar_device.data);
    unregister_chrdev_region(simplechar_devno, 1);
    printk(KERN_INFO "simplechar: Module unloaded\n");
}

module_init(simplechar_init);
module_exit(simplechar_exit);
