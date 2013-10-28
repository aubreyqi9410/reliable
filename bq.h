/*
 * Buffer Queue
 */

typedef struct bq {
    void* element_buffer;
    char* element_available;
    int num_elements;
    int element_size;
    int head;
    int head_seq;
} bq_t;

bq_t* bq_new(int num_elements, int element_size);
int bq_destroy(bq_t* bq);

int bq_insert_at(bq_t* bq, int index, void* element);

int bq_element_available(bq_t* bq, int index);

int bq_get_element_copy(bq_t* bq, int index, void* buf);
void *bq_get_element(bq_t* bq, int index);

int bq_get_head_seq(bq_t* bq);
int bq_get_tail_seq(bq_t* bq);

int bq_increase_head_seq_to(bq_t* bq, int index);
