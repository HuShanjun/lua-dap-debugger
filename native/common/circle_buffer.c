#include "circle_buffer.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct cb_block {
    _Atomic uint32_t read_pos;
    _Atomic uint32_t write_pos;
    _Atomic(struct cb_block *) next;
} cb_block;

struct circle_buffer {
    uint32_t block_size;
    uint32_t payload_cap;
    int free_empty;
    _Atomic(cb_block *) read_buf;
    _Atomic(cb_block *) write_buf;
    _Atomic uint64_t push_count;
    _Atomic uint64_t pop_count;
    uint8_t *scratch;
    size_t scratch_cap;
};

static uint8_t *cb_payload(cb_block *b) {
    return (uint8_t *)b + sizeof(cb_block);
}

static cb_block *cb_alloc_block(uint32_t block_size) {
    cb_block *b = (cb_block *)calloc(1, block_size);
    if (!b)
        return NULL;
    atomic_init(&b->read_pos, 0);
    atomic_init(&b->write_pos, 0);
    atomic_init(&b->next, (cb_block *)NULL);
    return b;
}

static void cb_free_block(cb_block *b) {
    free(b);
}

circle_buffer *circle_buffer_create(uint32_t block_size, int free_empty_buffer) {
    if (block_size <= (uint32_t)sizeof(cb_block))
        return NULL;

    circle_buffer *cb = (circle_buffer *)calloc(1, sizeof(*cb));
    if (!cb)
        return NULL;

    cb->block_size = block_size;
    cb->payload_cap = block_size - (uint32_t)sizeof(cb_block);
    cb->free_empty = free_empty_buffer ? 1 : 0;
    atomic_init(&cb->push_count, 0);
    atomic_init(&cb->pop_count, 0);
    cb->scratch = NULL;
    cb->scratch_cap = 0;

    cb_block *w = cb_alloc_block(block_size);
    cb_block *n = cb_alloc_block(block_size);
    if (!w || !n) {
        cb_free_block(w);
        cb_free_block(n);
        free(cb);
        return NULL;
    }
    atomic_store_explicit(&n->next, w, memory_order_relaxed);
    atomic_store_explicit(&w->next, n, memory_order_relaxed);
    atomic_init(&cb->write_buf, w);
    atomic_init(&cb->read_buf, w);
    (void)cb_payload; /* used by later tasks */
    return cb;
}

void circle_buffer_destroy(circle_buffer *cb) {
    if (!cb)
        return;
    cb_block *start = atomic_load_explicit(&cb->write_buf, memory_order_relaxed);
    if (start) {
        cb_block *cur = start;
        for (;;) {
            cb_block *next = atomic_load_explicit(&cur->next, memory_order_relaxed);
            cb_free_block(cur);
            if (next == start)
                break;
            cur = next;
        }
    }
    free(cb->scratch);
    free(cb);
}

/* stubs so later tasks compile if linked early — remove as functions are filled */
int circle_buffer_push_raw(circle_buffer *cb, const circle_buf *bufs, uint32_t count) {
    (void)cb; (void)bufs; (void)count; return -1;
}
int circle_buffer_push_raw_one(circle_buffer *cb, const void *data, uint32_t size) {
    (void)cb; (void)data; (void)size; return -1;
}
uint32_t circle_buffer_pop_raw(circle_buffer *cb, void *out, uint32_t out_size) {
    (void)cb; (void)out; (void)out_size; return 0;
}
int circle_buffer_push_buffer(circle_buffer *cb, const void *context, size_t context_size,
                              const circle_buf *bufs, uint32_t count, int merge) {
    (void)cb; (void)context; (void)context_size; (void)bufs; (void)count; (void)merge;
    return -1;
}
int circle_buffer_pop_buffer(circle_buffer *cb, void *context_out, size_t context_size,
                             circle_buf *bufs, uint32_t *inout_count) {
    (void)cb; (void)context_out; (void)context_size; (void)bufs; (void)inout_count;
    return 0;
}
int circle_buffer_can_pop(const circle_buffer *cb) {
    (void)cb; return 0;
}
uint32_t circle_buffer_waiting_count(const circle_buffer *cb) {
    (void)cb; return 0;
}
