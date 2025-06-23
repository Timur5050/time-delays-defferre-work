#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/wait.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Timur");
MODULE_DESCRIPTION("A simple char device driver with single device");
MODULE_VERSION("1.0");

struct simplechar_dev {
    char *data;            
    unsigned long size;    
    unsigned long delay_ms; // delay in read (ig long delays)
    unsigned long udelay_us; // delay in write (short delays)
    unsigned long ndelay_ns; // delay in write (shrt delays)
    unsigned long total_delay_ns; // stat of delays
    wait_queue_head_t waitq; // queue for long delays
    int data_ready; // condition for wait event
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
    char tmp_buf[BUFFER_SIZE];
    ssize_t retval = 0;
    int len;

    if (dev->delay_ms) {
        int ret = wait_event_interruptible_timeout(
            dev->waitq,
            dev->data_ready,
            msecs_to_jiffies(dev->delay_ms)
        );

        if (ret == 0) {
            printk(KERN_INFO "simplechar: read timeout\n");
            return 0;
        }
        if (ret < 0) {
            printk(KERN_INFO "simplechar: interrupted while sleeping\n");
            return -EINTR;
        }
        dev->data_ready = 0;
    }

    if (dev->size == 0)
        return 0;

    if (*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    if (dev->size == 0 || *f_pos >= dev->size) {
        return 0; 
    }

    len = snprintf(tmp_buf, BUFFER_SIZE,
        "data: %.*s\n"
        "total_delay_ns: %lu\n",
        (int)count, dev->data + *f_pos, dev->total_delay_ns);

    if (copy_to_user(buf, tmp_buf, len)) {
        printk(KERN_ERR "simplechar: Failed to copy data to user\n");
        return -EFAULT;
    }

    *f_pos += len;
    if (*f_pos >= dev->size) return 0;
    retval = len;
    printk(KERN_INFO "simplechar: Read %zd bytes from pos %lld\n", retval, *f_pos);
    return retval;
}


static ssize_t simplechar_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct simplechar_dev *dev = filp->private_data;
    char tmp_buf[BUFFER_SIZE];
    unsigned long new_delay_ms, new_udelay_us, new_ndelay_ns;
    ssize_t i;

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

    tmp_buf[count - 1] = '\0';

    if(strncmp(tmp_buf, "reset", 5) == 0)
    {
        dev->delay_ms = 0;
        dev->udelay_us = 0;
        dev->ndelay_ns = 0;
        dev->total_delay_ns = 0;
        dev->data_ready = 0;
        dev->size = 0;
        memset(dev->data, 0, BUFFER_SIZE);
        return count;
    }

    if(sscanf(tmp_buf, "delay_ms=%lu", &new_delay_ms) == 1)
    {
        dev->delay_ms = new_delay_ms;
        return count;
    }

    if(sscanf(tmp_buf, "udelay_us=%lu", &new_udelay_us) == 1)
    {
        if(new_udelay_us > 1000) // secure from __bad_udelay
        {
            return -EINVAL;
        }
        dev->udelay_us = new_udelay_us;
        return count;
    }

    if(sscanf(tmp_buf, "ndelays_ns=%lu", &new_ndelay_ns) == 1)
    {
        if(new_ndelay_ns > 1000000) // secure from long ndelay
        {
            return -EINVAL;
        }
        dev->ndelay_ns = new_ndelay_ns;
        return count;
    }

    for(i = 0; i < count; i++)
    {
        if(dev->udelay_us)
        {
            udelay(dev->udelay_us);
        }
        if(dev->ndelay_ns)
        {
            ndelay(dev->ndelay_ns);
        }
        dev->total_delay_ns +=(dev->udelay_us * 1000) + dev->ndelay_ns;
    }

    memcpy(dev->data + *f_pos, tmp_buf, count);

    *f_pos += count;
    if (dev->size < *f_pos) 
        dev->size = *f_pos;

    dev->data_ready = 1;
    wake_up_interruptible(&dev->waitq);

    printk(KERN_INFO "simplechar: Wrote %zd bytes to pos %lld\n", count, *f_pos);
    return count;
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

    err = alloc_chrdev_region(&simplechar_devno, 0, 1, "simplechardelay");

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
    simplechar_device.delay_ms = 0;
    simplechar_device.udelay_us = 0;
    simplechar_device.ndelay_ns = 0;
    simplechar_device.total_delay_ns = 0;
    simplechar_device.data_ready = 0;
    init_waitqueue_head(&simplechar_device.waitq);
    memset(simplechar_device.data, 0, BUFFER_SIZE);

    cdev_init(&simplechar_device.cdev, &simplechar_fops);
    simplechar_device.cdev.owner = THIS_MODULE;
    err = cdev_add(&simplechar_device.cdev, simplechar_devno, 1);
    if (err) {
        printk(KERN_ERR "simplechar: Failed to add cdev\n");
        goto fail_cdev;
    }

    simplechar_class = class_create("simplechardelay");
    if (IS_ERR(simplechar_class)) {
        err = PTR_ERR(simplechar_class);
        printk(KERN_ERR "simplechar: Failed to create class\n");
        goto fail_class;
    }
    device_create(simplechar_class, NULL, simplechar_devno, NULL, "simplechardelay");

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