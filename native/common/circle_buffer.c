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

static int write_bytes(circle_buffer *cb, cb_block **p_write, uint32_t *p_wpos,
                       cb_block *read_buf, const void *src, size_t n) {
    const uint8_t *cur = (const uint8_t *)src;
    while (n) {
        uint32_t left = cb->payload_cap - *p_wpos;
        uint32_t chunk = (uint32_t)((n < left) ? n : left);
        memcpy(cb_payload(*p_write) + *p_wpos, cur, chunk);
        cur += chunk;
        *p_wpos += chunk;
        n -= chunk;

        if (*p_wpos != cb->payload_cap) {
            cb_block *next = atomic_load_explicit(&(*p_write)->next, memory_order_acquire);
            if (cb->free_empty && next != read_buf && next != *p_write) {
                cb_block *check = atomic_load_explicit(&next->next, memory_order_acquire);
                while (check != read_buf && check != *p_write) {
                    cb_block *victim = check;
                    check = atomic_load_explicit(&victim->next, memory_order_acquire);
                    atomic_store_explicit(&next->next, check, memory_order_release);
                    cb_free_block(victim);
                }
            }
            break;
        }

        cb_block *cur_next = atomic_load_explicit(&(*p_write)->next, memory_order_acquire);
        if (cur_next == read_buf) {
            cb_block *neu = cb_alloc_block(cb->block_size);
            if (!neu)
                return -1;
            atomic_store_explicit(&neu->next, read_buf, memory_order_relaxed);
            atomic_store_explicit(&(*p_write)->next, neu, memory_order_release);
            cur_next = neu;
        }
        *p_write = cur_next;
        *p_wpos = 0;
    }
    return 0;
}

static void publish_write(circle_buffer *cb, cb_block *start, cb_block *end, uint32_t wpos) {
    while (start != end) {
        atomic_store_explicit(&start->write_pos, cb->payload_cap, memory_order_release);
        start = atomic_load_explicit(&start->next, memory_order_acquire);
    }
    atomic_store_explicit(&end->write_pos, wpos, memory_order_release);
    atomic_store_explicit(&cb->write_buf, end, memory_order_release);
    atomic_fetch_add_explicit(&cb->push_count, 1, memory_order_relaxed);
}

static int begin_write(circle_buffer *cb, cb_block **start, cb_block **end,
                       cb_block **read_buf, uint32_t *wpos) {
    *start = atomic_load_explicit(&cb->write_buf, memory_order_relaxed);
    *end = *start;
    *read_buf = atomic_load_explicit(&cb->read_buf, memory_order_acquire);
    *wpos = atomic_load_explicit(&(*start)->write_pos, memory_order_relaxed);
    return 0;
}

int circle_buffer_push_raw(circle_buffer *cb, const circle_buf *bufs, uint32_t count) {
    if (!cb || count == 0)
        return 0;
    cb_block *start, *end, *read_buf;
    uint32_t wpos;
    begin_write(cb, &start, &end, &read_buf, &wpos);
    for (uint32_t i = 0; i < count; i++) {
        if (write_bytes(cb, &end, &wpos, read_buf, bufs[i].data, bufs[i].size) != 0)
            return -1;
    }
    publish_write(cb, start, end, wpos);
    return 0;
}

int circle_buffer_push_raw_one(circle_buffer *cb, const void *data, uint32_t size) {
    circle_buf one = { data, size };
    return circle_buffer_push_raw(cb, &one, 1);
}

uint32_t circle_buffer_pop_raw(circle_buffer *cb, void *out, uint32_t out_size) {
    if (!cb || !out || out_size == 0)
        return 0;

    cb_block *write_buf = atomic_load_explicit(&cb->write_buf, memory_order_acquire);
    uint32_t write_pos = atomic_load_explicit(&write_buf->write_pos, memory_order_acquire);
    if (write_pos == cb->payload_cap)
        return 0;

    cb_block *read_buf = atomic_load_explicit(&cb->read_buf, memory_order_relaxed);
    uint32_t read_pos = atomic_load_explicit(&read_buf->read_pos, memory_order_relaxed);
    if (read_buf == write_buf && read_pos == write_pos)
        return 0;

    uint8_t *dst = (uint8_t *)out;
    uint32_t remain = out_size;
    while (remain) {
        uint32_t cur_w = atomic_load_explicit(&read_buf->write_pos, memory_order_acquire);
        uint32_t left = cur_w - read_pos;
        if (left == 0)
            break;
        uint32_t n = remain < left ? remain : left;
        memcpy(dst, cb_payload(read_buf) + read_pos, n);
        dst += n;
        read_pos += n;
        remain -= n;
        if (read_pos != cb->payload_cap)
            break;
        read_buf = atomic_load_explicit(&read_buf->next, memory_order_acquire);
        read_pos = 0;
    }

    atomic_store_explicit(&read_buf->read_pos, read_pos, memory_order_release);
    atomic_store_explicit(&cb->read_buf, read_buf, memory_order_release);
    atomic_fetch_add_explicit(&cb->pop_count, 1, memory_order_relaxed);
    return (uint32_t)(dst - (uint8_t *)out);
}

int circle_buffer_can_pop(const circle_buffer *cb) {
    if (!cb)
        return 0;
    cb_block *write_buf = atomic_load_explicit(&cb->write_buf, memory_order_acquire);
    uint32_t write_pos = atomic_load_explicit(&write_buf->write_pos, memory_order_acquire);
    if (write_pos == cb->payload_cap)
        return 0;
    cb_block *read_buf = atomic_load_explicit(&cb->read_buf, memory_order_acquire);
    uint32_t read_pos = atomic_load_explicit(&read_buf->read_pos, memory_order_acquire);
    if (read_buf == write_buf && read_pos == write_pos)
        return 0;
    return 1;
}

uint32_t circle_buffer_waiting_count(const circle_buffer *cb) {
    if (!cb)
        return 0;
    uint64_t p = atomic_load_explicit(&cb->push_count, memory_order_acquire);
    uint64_t q = atomic_load_explicit(&cb->pop_count, memory_order_acquire);
    return (uint32_t)(p - q);
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
