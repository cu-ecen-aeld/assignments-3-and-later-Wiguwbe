/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    bool out_eq_in = buffer->out_offs == buffer->in_offs;
    uint8_t read_offs = buffer->out_offs;
    // make sure we don't over-iterate
    // this also helps with full buffer
    uint8_t i;
    for(i=0; i<AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;i++)
    {
        if(
            read_offs == buffer->in_offs
            &&
            (
                ( out_eq_in && !buffer->full)
                ||
                !out_eq_in
            )
        )
        {
            /*
                NOTES REGARDING THIS CHECK

                With the enclosing for loop, we guarantee that we will not
                rollover to re-read already visited nodes, so,
                if we catch up with `in_offs` it's either:

                - `out_offs == in_offs`, which, because of the enclosing for loop
                will only happen if it's in the beggining (will break before we reach it again)
                and, if the buffer is not full, then it's empty

                or

                - `out_offs != in_offs`, which means we reached the end.
            */
            // means we didn't find it
            break;
        }
        if(char_offset < buffer->entry[read_offs].size)
        {
            // cool, let's read it
            *entry_offset_byte_rtn = char_offset;
            return buffer->entry+read_offs;
        }
        // else, continue searching
        char_offset -= buffer->entry[read_offs].size;
        read_offs ++;
        if(read_offs == AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED)
            read_offs = 0;
    }
    // (for) else, it wasn't found
    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
char *aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    char *ret_value = buffer->entry->buffptr;
    // copy
    memcpy(buffer->entry+buffer->in_offs, add_entry, sizeof(struct aesd_buffer_entry));
    // increment
    buffer->in_offs ++;
    // rollover
    if(buffer->in_offs == AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED)
        buffer->in_offs = 0;
    if(buffer->full)
        // also increment out_offs
        buffer->out_offs = buffer->in_offs;
    // check for catch-up (no need to check if we know it's already full)
    else if(buffer->in_offs == buffer->out_offs)
        buffer->full = true;
    return ret_value;
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
