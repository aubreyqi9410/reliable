#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "bq.h"

/* 
 * Private 
 */

int bq_contains_index(bq_t* bq, int index)
{
    return (index >= bq_get_head_seq(bq)) && (index <= bq_get_tail_seq(bq));
}

int bq_index_to_offset(bq_t* bq, int index)
{
    assert(index >= bq_get_head_seq(bq) && index >= 0);
    return index % bq->num_elements;
}

/* 
 * Public 
 */

bq_t* bq_new(int num_elements, int element_size) 
{
    bq_t* bq = (bq_t*)malloc(sizeof(bq_t));

    bq->element_buffer = calloc(num_elements, element_size);
    bq->element_buffered = calloc(num_elements, sizeof(int));

    bq->num_elements = num_elements;
    bq->element_size = element_size;

    bq->head = 0;
    bq->head_seq = 0;

    return bq;
}

int bq_destroy(bq_t* bq)
{
    free(bq->element_buffer);
    free(bq->element_buffered);
    free(bq);
    return 0;
}

int bq_insert_at(bq_t* bq, int index, void* element)
{
    if (!bq_contains_index(bq, index)) return -1;

    memcpy(bq_get_element(bq, index), element, bq->element_size);
    bq->element_buffered[bq_index_to_offset(bq, index)] = 1;

    return 0;
}

int bq_element_buffered(bq_t* bq, int index)
{
    return bq->element_buffered[bq_index_to_offset(bq, index)];
}

void *bq_get_element(bq_t* bq, int index)
{
    return bq->element_buffer + (bq_index_to_offset(bq, index) * bq->element_size);
}

int bq_get_head_seq(bq_t* bq)
{
    return bq->head_seq;
}

int bq_get_tail_seq(bq_t* bq)
{
    return bq->head_seq + bq->num_elements - 1;
}

int bq_increase_head_seq_to(bq_t* bq, int index)
{
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
