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
#include "aesd_ioctl.h"

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
    filp->private_data = inode->i_cdev;
    // all good then
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    // nothing to release
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
    struct aesd_dev *aesd_dev = (struct aesd_dev*)filp->private_data;
    // acquire read semaphore
    down_read(&aesd_dev->semaphore);
    // while space_left > 0
    while(space_left > 0)
    {
        //PDEBUG("got %d space left", space_left);
        // if find_entry_for_offset == NULL -> return total_read
        if((buffer_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&aesd_dev->circular_buffer, read_offset, &entry_offset)) == NULL)
            // EOF
            break;
        // else
        // copy to user (min(space_left, size))
        //PDEBUG("buffer entry is at %p: (%d) %s", buffer_entry, buffer_entry->size, buffer_entry->buffptr);
        size_t left_in_entry = buffer_entry->size - entry_offset;
        size_t to_copy = space_left > left_in_entry ? left_in_entry : space_left;
        //PDEBUG("copying %d bytes to user", to_copy);
        size_t copied = to_copy - copy_to_user(buf + total_read, buffer_entry->buffptr + entry_offset, to_copy);
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
    up_read(&aesd_dev->semaphore);
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
    struct aesd_dev *aesd_dev = (struct aesd_dev*)filp->private_data;
    if(mutex_lock_interruptible(&aesd_dev->save_mutex))
        return -EINTR;
    // append to buffer
    {
        // save previous
        char *prev = aesd_dev->buffer_entry.buffptr;
        // kmalloc with added size
        aesd_dev->buffer_entry.buffptr = kmalloc(aesd_dev->buffer_entry.size + count, GFP_KERNEL);
        if(!aesd_dev->buffer_entry.buffptr)
        {
            aesd_dev->buffer_entry.buffptr = prev;    // restore for `release` operation
            mutex_unlock(&aesd_dev->save_mutex);
            return -ENOMEM;
        }
        //PDEBUG("write buffer is at %p, prev was %p", fd->buffer_entry.buffptr, prev);
        if(prev)
        {
            // copy first portion
            memcpy(aesd_dev->buffer_entry.buffptr, prev, aesd_dev->buffer_entry.size);
            // free previous
            kfree(prev);
        }
        // append new data (return number of bytes copied)
        retval = count - copy_from_user(aesd_dev->buffer_entry.buffptr+aesd_dev->buffer_entry.size, buf, count);
        aesd_dev->buffer_entry.size += retval;
    }
    // check for '\n'
    {
        char *newline = memchr(aesd_dev->buffer_entry.buffptr, '\n', aesd_dev->buffer_entry.size);
        if(!newline)
            goto _ret;
        // else, write to circular_buffer
        // (assume '\n' is the final character)
        if((newline+1-aesd_dev->buffer_entry.buffptr) != aesd_dev->buffer_entry.size)
            PDEBUG("there are characters after command terminator (newline)");
        aesd_dev->buffer_entry.size = newline + 1 - aesd_dev->buffer_entry.buffptr;
        // acquire semaphore to write
        down_write(&aesd_dev->semaphore);
        // write
        char *prev_buffer = aesd_circular_buffer_add_entry(&aesd_dev->circular_buffer, &aesd_dev->buffer_entry);
        // release
        up_write(&aesd_dev->semaphore);
        // free previous
        if(prev_buffer) kfree(prev_buffer);
        // clean it after copy
        aesd_dev->buffer_entry.buffptr = NULL;
        aesd_dev->buffer_entry.size = 0;
    }
_ret:
    mutex_unlock(&aesd_dev->save_mutex);
    return retval;
}

loff_t aesd_llseek(struct file * filp, loff_t offset, int whence)
{
    loff_t newpos = 0, buffer_size;
    struct aesd_dev *aesd_dev = (struct aesd_dev*)filp->private_data;
    // don't allow writes while we calculate stuff
    /*
        none of the functions we use here block
        (at the time of writing, at least, good luck in the future)
    */
    down_read(&aesd_dev->semaphore);
    buffer_size = aesd_circular_buffer_len(&aesd_dev->circular_buffer);
    switch(whence)
    {
        case SEEK_SET:
            newpos = offset;
            break;
        case SEEK_CUR:
            newpos = filp->f_pos + offset;
            break;
        case SEEK_END:
            newpos = buffer_size + offset;
            break;
        default:
            newpos = -EINVAL;
            goto _end;
    }
    // don't allow overshoot (aswell as negative offset)
    if(newpos < 0 || newpos > buffer_size)
    {
        newpos = -EINVAL;
        goto _end;
    }
    // set it
    filp->f_pos = newpos;
_end:
    up_read(&aesd_dev->semaphore);
    return newpos;
}

long aesd_u_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_seekto req;
    struct aesd_buffer_entry *entry;
    struct aesd_dev *aesd_dev = (struct aesd_dev*)filp->private_data;
    loff_t newpos;
    unsigned long long entry_offset;

    PDEBUG("ioctl command: %d, %d", _IOC_TYPE(cmd), _IOC_NR(cmd));

    if(_IOC_TYPE(cmd) != AESD_IOC_MAGIC || _IOC_NR(cmd) > AESDCHAR_IOC_MAXNR)
        return -ENOTTY;

    switch(cmd)
    {
        case AESDCHAR_IOCSEEKTO:
        {
            if(copy_from_user(&req, (void*)arg, sizeof(req)))
                // couldn't read all
                return -EINVAL;
            // don't allow writes while we update this
            down_read(&aesd_dev->semaphore);
            entry = aesd_circular_buffer_get_entry_no(&aesd_dev->circular_buffer, req.write_cmd, &entry_offset);
            if(!entry || req.write_cmd_offset >= entry->size)
            {
                up_read(&aesd_dev->semaphore);
                return -EINVAL;
            }
            newpos = (loff_t)(entry_offset + req.write_cmd_offset);
            // set it
            filp->f_pos = newpos;
            up_read(&aesd_dev->semaphore);
            // return normally
        }
        break;
        default:
            return -ENOTTY;
    }

    return 0;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .llseek =   aesd_llseek,
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
    mutex_init(&aesd_device.save_mutex);

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
    if(aesd_device.buffer_entry.buffptr)
        kfree(aesd_device.buffer_entry.buffptr);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
