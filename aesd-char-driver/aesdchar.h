/*
 * aesdchar.h
 *
 *  Created on: Oct 23, 2019
 *      Author: Dan Walkes
 */

#ifndef AESD_CHAR_DRIVER_AESDCHAR_H_
#define AESD_CHAR_DRIVER_AESDCHAR_H_

#define AESD_DEBUG 1  //Remove comment on this line to enable debug

#undef PDEBUG             /* undef it, just in case */
#ifdef AESD_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_DEBUG "aesdchar: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

#include "aesd-circular-buffer.h"

struct aesd_dev
{
    /*
        if we set this as the first member, we don't need to
        use `container_of` :)
        (a pointer to this cdev is also a pointer to aesd_dev)
    */
    struct cdev cdev;     /* Char device structure      */

    /**
     * TODO: Add structure(s) and locks needed to complete assignment requirements
     */
    // the circular buffer
    struct aesd_circular_buffer circular_buffer;
    // for read write stuff
    struct rw_semaphore semaphore;

    // save buffer
    struct aesd_buffer_entry buffer_entry;
    struct mutex save_mutex;
};



#endif /* AESD_CHAR_DRIVER_AESDCHAR_H_ */
