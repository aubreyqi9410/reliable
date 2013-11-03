#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "bq.h"

/* 
 * Private 
 */

/* Returns whether or not a buffer queue's current memory window contains
 * an index. 1 for true, 0 for false.
 */

int bq_contains_index(bq_t* bq, int index)
{
    assert(bq);

    return (index >= bq_get_head_seq(bq)) && (index <= bq_get_tail_seq(bq));
}

/* Returns an offset into the memory window for a given index. You must already
 * have checked that bq_contains_index, or an assert could fail.
 */

int bq_index_to_offset(bq_t* bq, int index)
{
    assert(bq);
    assert(index >= bq_get_head_seq(bq) && index >= 0);

    return index % bq->num_elements;
}

/* 
 * Public 
 */

/* Allocates a new buffer queue. If any of the system calls fail, asserts will fail,
 * and the function will crash.
 */

bq_t* bq_new(int num_elements, int element_size) 
{
    assert(num_elements > 0);
    assert(element_size > 0);

    bq_t* bq = (bq_t*)malloc(sizeof(bq_t));
    assert(bq);

    bq->element_buffer = calloc(num_elements, element_size);
    assert(bq->element_buffer);
    bq->element_buffered = calloc(num_elements, sizeof(int));
    assert(bq->element_buffered);

    bq->num_elements = num_elements;
    bq->element_size = element_size;

    bq->head = 0;
    bq->head_seq = 0;

    return bq;
}

/* Double the memory size of the buffer, to make some more
 * space for shtuff.
 */

void bq_double_size(bq_t* bq)
{
    assert(bq);

    void* new_element_buffer = calloc(bq->num_elements * 2, bq->element_size);
    assert(new_element_buffer);
    void* new_element_buffered = calloc(bq->num_elements * 2, sizeof(int));
    assert(new_element_buffered);

    /* We have to move elements one at a time, because the modulo
     * indexing can mess things up if we just copy in a block. */

    int i;
    for (i = bq_get_head_seq(bq); i <= bq_get_tail_seq(bq); i++) {
        if (bq_element_buffered(bq,i)) {
            int new_index = i % (bq->num_elements * 2);
            memcpy(new_element_buffer + (new_index * bq->element_size), bq_get_element(bq,i), bq->element_size);
            new_element_buffered[new_index] = 1;
        }
    }

    free(bq->element_buffer);
    free(bq->element_buffered);

    bq->element_buffer = new_element_buffer;
    bq->element_buffered = new_element_buffered;
    bq->num_elements = bq->num_elements*2;
}

/* Frees all the memory associated with a buffer queue.
 */

int bq_destroy(bq_t* bq)
{
    assert(bq);

    free(bq->element_buffer);
    free(bq->element_buffered);
    free(bq);
    return 0;
}

/* memcpy's and element into the buffer queue at an index.
 * Assumes the element is a pointer to a block of memory that
 * is the size of an element in the buffer queue. Returns 0
 * on success, and -1 on failure (because you asked for an
 * OOB index)
 */

int bq_insert_at(bq_t* bq, int index, void* element)
{
    assert(bq);
    assert(element);

    if (!bq_contains_index(bq, index)) return -1;

    memcpy(bq_get_element(bq, index), element, bq->element_size);
    bq->element_buffered[bq_index_to_offset(bq, index)] = 1;

    return 0;
}

/* Checks if an element is buffered in the buffer queue. This
 * is important to check, because a get_element will always
 * return a pointer to memory, and buffer queue doesn't explicitly
 * protect you from getting elements that you didn't actually
 * put there.
 */

int bq_element_buffered(bq_t* bq, int index)
{
    assert(bq);

    if (!bq_contains_index(bq, index)) return 0;

    return bq->element_buffered[bq_index_to_offset(bq, index)];
}

/* Returns a pointer to an element on the buffer queue memory
 * segment. Not thread safe. Useful because it allows you to
 * make modifications without having to call insert_at, and
 * suffer a memcpy.
 */

void *bq_get_element(bq_t* bq, int index)
{
    assert(bq);

    return bq->element_buffer + (bq_index_to_offset(bq, index) * bq->element_size);
}

/* Returns the head index of the buffer queue abstraction (where
 * the infinite memory segment becomes real).
 */

int bq_get_head_seq(bq_t* bq)
{
    assert(bq);
    
    return bq->head_seq;
}

/* Returns the tail index of the buffer queue abstraction (where
 * the infinite memory segment stops being real).
 */

int bq_get_tail_seq(bq_t* bq)
{
    assert(bq);

    return bq->head_seq + bq->num_elements - 1;
}

/* Move the head of the infinite buffer up, clearing out any entries
 * that the head passes over on its way up, because they are no longer
 * in the memory window. It doesn't actually erase the memory, just makes
 * a note that those segments are invalid. Always use element_buffered to
 * check before you get an element.
 */

int bq_increase_head_seq_to(bq_t* bq, int index)
{
    assert(bq);

    if (index <= bq->head_seq) return -1;

    /* invalidate all entries that the head passes over, because
     * we will be using those slots to hold higher sequence numers
     * now.
     */
    int i;
    for (i = bq->head_seq; i < index; i++) {
        bq->element_buffered[bq_index_to_offset(bq, i)] = 0;
    }

    bq->head_seq = index;
    bq->head = index % bq->num_elements;

    return 0;
}
