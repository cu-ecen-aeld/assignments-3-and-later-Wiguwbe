/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/rwsem.h>
#include <linux/string.h>
#include <linux/slab.h>
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Tiago Teixeira"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct filp_data *filp_data = kmalloc(sizeof(struct filp_data), GFP_KERNEL);
    if(!filp_data)
        return -ENOMEM;
    memset(filp_data, 0, sizeof(struct filp_data));
    filp_data->aesd_dev = (struct aesd_dev*)inode->i_cdev;
    filp->private_data = filp_data;
    // all good then
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    struct filp_data *fd = (struct filp_data*)filp->private_data;
    if(fd->buffer_entry.buffptr)    // this shouldn't happen
        kfree(fd->buffer_entry.buffptr);
    kfree(fd);
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    size_t total_read = 0,
           entry_offset = 0,
           space_left = count;
    size_t read_offset = (size_t)*f_pos;
    struct aesd_buffer_entry *buffer_entry;
    struct filp_data *fd = (struct filp_data*)filp->private_data;
    // acquire read semaphore
    down_read(&fd->aesd_dev->semaphore);
    // while space_left > 0
    while(space_left > 0)
    {
        // if find_entry_for_offset == NULL -> return total_read
        if((buffer_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&fd->aesd_dev->circular_buffer, read_offset, &entry_offset)) == NULL)
            // EOF
            break;
        // else
        // copy to user (min(space_left, size))
        size_t left_in_entry = buffer_entry->size - entry_offset;
        size_t to_copy = space_left > left_in_entry ? left_in_entry : space_left;
        size_t copied = copy_to_user(buf + total_read, buffer_entry->buffptr + entry_offset, to_copy);
        // total_read += ?
        total_read += copied;
        read_offset += copied;
        // space_left -= read
        space_left -= copied;
        if(copied != to_copy)
        {
            // failed to copy full data to user,
            // let's just return what we already have
            // (hopefully it's not zero)
            break;
        }
    }
    // release semaphore
    up_read(&fd->aesd_dev->semaphore);
    *f_pos = (loff_t)read_offset;
    return (ssize_t)total_read;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
    struct filp_data *fd = (struct filp_data*)filp->private_data;
    // append to buffer
    {
        // save previous
        char *prev = fd->buffer_entry.buffptr;
        // kmalloc with added size
        fd->buffer_entry.buffptr = kmalloc(fd->buffer_entry.size + count, GFP_KERNEL);
        if(!fd->buffer_entry.buffptr)
        {
            fd->buffer_entry.buffptr = prev;    // restore for `release` operation
            return -ENOMEM;
        }
        if(prev)
        {
            // copy first portion
            memcpy(fd->buffer_entry.buffptr, prev, fd->buffer_entry.size);
            // free previous
            kfree(prev);
        }
        // append new data (return number of bytes copied)
        retval = copy_from_user(fd->buffer_entry.buffptr+fd->buffer_entry.size, buf, count);
        fd->buffer_entry.size += retval;
    }
    // check for '\n'
    {
        char *newline = memchr(fd->buffer_entry.buffptr, '\n', fd->buffer_entry.size);
        if(!newline)
            goto _ret;
        // else, write to circular_buffer
        // (assume '\n' is the final character)
        if((newline+1-fd->buffer_entry.buffptr) != fd->buffer_entry.size)
            PDEBUG("there are characters after command terminator (newline)");
        fd->buffer_entry.size = newline + 1 - fd->buffer_entry.buffptr;
        // acquire semaphore to write
        down_write(&fd->aesd_dev->semaphore);
        // write
        char *prev_buffer = aesd_circular_buffer_add_entry(&fd->aesd_dev->circular_buffer, &fd->buffer_entry);
        // release
        up_write(&fd->aesd_dev->semaphore);
        // free previous
        if(prev_buffer) kfree(prev_buffer);
    }
_ret:
    return retval;
}
struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    init_rwsem(&aesd_device.semaphore);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    // nothing to be cleaned

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);