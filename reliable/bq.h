/*
 * BUFFER QUEUE
 *
 * by Keenon Werling, for CS144, Oct 29, 2013
 *
 * The queue holds entries from a head index (starts at 0) up to
 * head index + queue capacity. A user can advance the head index, 
 * clearing out entries below the head index, and making space for
 * more entries, without doing memmoves, so there aren't very many
 * moving pieces (literally or figuratively).
 *
 * Internally, it's implemented using a modulo index into a block
 * of memory.
 *
 * QUEUE ABSTRACTION:                    ACTUAL MEMORY:
 * -----------                                 ---------------
 *  | 0 | gone forever                  |----> | 0 | queue index 4
 * -----------                          |      ---------------
 *  | 1 | gone forever                  | |--> | 1 | queue index 5
 * -----------                          | |    ---------------
 *  | 2 | (head) mem index = 2 % 3 = 2 -|-|--> | 2 | queue index 3
 * -----------                          | |    ---------------
 *  | 3 | mem index = 3 % 3 = 0 --------| |
 * -----------                            |
 *  | 4 | mem index = 4 % 3 = 1 ----------|
 * -----------
 *  | 5 | not yet accessible
 * -----------
 *  | 6 | not yet accessible
 */

typedef struct bq {
    void* element_buffer;
    int* element_buffered;
    int num_elements;
    int element_size;
    int head;
    int head_seq;
} bq_t;

/* Create and destroy a buffer queue */

bq_t* bq_new(int num_elements, int element_size);
int bq_destroy(bq_t* bq);

/**
 * Doubles the size fo the buffer, useful if a buffer overrun
 * is about to happen, and more space needs to be made. A bit
 * imprecise, but it keeps things simple.
 */

void bq_double_size(bq_t* bq);

/**
 * Inserts an element into the queue at the requested index.
 * If that index is out of bounds, returns 0. Else overwrites
 * whatever is in that index, and returns 1.
 */

int bq_insert_at(bq_t* bq, int index, void* element);

/**
 * Checks if an element has been buffered in the buffer queue.
 * It's possible to access elements that haven't been put into
 * the queue, so it is the users responsibility to check this
 * first.
 */

int bq_element_buffered(bq_t* bq, int index);

/**
 * Get a pointer to an element in the queue. Any modifications
 * of this element will be done in the queue's memory segment,
 * and so will be reflected to other users. Not thread safe.
 */

void *bq_get_element(bq_t* bq, int index);

/**
 * Get the head and tail of the accessible memory addressed by the
 * queue abstraction.
 */

int bq_get_head_seq(bq_t* bq);
int bq_get_tail_seq(bq_t* bq);

/**
 * Increase the head index to a value, clearing out all entries below
 * it, and making space available for more entries at higher indexes.
 */

int bq_increase_head_seq_to(bq_t* bq, int index);
