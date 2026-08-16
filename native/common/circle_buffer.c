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

static int ensure_scratch(circle_buffer *cb, size_t need) {
    if (cb->scratch_cap >= need)
        return 0;
    uint8_t *p = (uint8_t *)realloc(cb->scratch, need);
    if (!p)
        return -1;
    cb->scratch = p;
    cb->scratch_cap = need;
    return 0;
}

static int read_bytes(circle_buffer *cb, cb_block **p_read, uint32_t *p_rpos,
                      void *dst, size_t n) {
    uint8_t *cur = (uint8_t *)dst;
    while (n) {
        uint32_t wpos = atomic_load_explicit(&(*p_read)->write_pos, memory_order_acquire);
        uint32_t left = wpos - *p_rpos;
        assert(left > 0);
        uint32_t chunk = (uint32_t)n;
        if (n > left) {
            assert(wpos == cb->payload_cap);
            chunk = left;
        }
        memcpy(cur, cb_payload(*p_read) + *p_rpos, chunk);
        cur += chunk;
        *p_rpos += chunk;
        n -= chunk;
        if (*p_rpos != cb->payload_cap)
            break;
        *p_read = atomic_load_explicit(&(*p_read)->next, memory_order_acquire);
        *p_rpos = 0;
    }
    return 0;
}

int circle_buffer_push_buffer(circle_buffer *cb, const void *context, size_t context_size,
                              const circle_buf *bufs, uint32_t count, int merge) {
    if (!cb)
        return -1;
    if (context_size && !context)
        return -1;

    cb_block *start, *end, *read_buf;
    uint32_t wpos;
    begin_write(cb, &start, &end, &read_buf, &wpos);

    if (context_size) {
        if (write_bytes(cb, &end, &wpos, read_buf, context, context_size) != 0)
            return -1;
    }

    if (merge && count) {
        uint32_t one = 1;
        if (write_bytes(cb, &end, &wpos, read_buf, &one, sizeof(one)) != 0)
            return -1;
        uint32_t total = 0;
        for (uint32_t i = 0; i < count; i++)
            total += bufs[i].size;
        if (write_bytes(cb, &end, &wpos, read_buf, &total, sizeof(total)) != 0)
            return -1;
        for (uint32_t i = 0; i < count; i++) {
            if (write_bytes(cb, &end, &wpos, read_buf, bufs[i].data, bufs[i].size) != 0)
                return -1;
        }
    } else {
        if (write_bytes(cb, &end, &wpos, read_buf, &count, sizeof(count)) != 0)
            return -1;
        for (uint32_t i = 0; i < count; i++) {
            if (write_bytes(cb, &end, &wpos, read_buf, &bufs[i].size, sizeof(uint32_t)) != 0)
                return -1;
            if (write_bytes(cb, &end, &wpos, read_buf, bufs[i].data, bufs[i].size) != 0)
                return -1;
        }
    }

    publish_write(cb, start, end, wpos);
    return 0;
}

int circle_buffer_pop_buffer(circle_buffer *cb, void *context_out, size_t context_size,
                             circle_buf *bufs, uint32_t *inout_count) {
    if (!cb || !inout_count || !bufs)
        return 0;

    cb_block *write_buf = atomic_load_explicit(&cb->write_buf, memory_order_acquire);
    uint32_t write_pos = atomic_load_explicit(&write_buf->write_pos, memory_order_acquire);
    if (write_pos == cb->payload_cap)
        return 0;

    cb_block *read_buf = atomic_load_explicit(&cb->read_buf, memory_order_relaxed);
    uint32_t read_pos = atomic_load_explicit(&read_buf->read_pos, memory_order_relaxed);
    if (read_buf == write_buf && read_pos == write_pos)
        return 0;

    if (context_size) {
        if (!context_out)
            return 0;
        read_bytes(cb, &read_buf, &read_pos, context_out, context_size);
    }

    uint32_t ncount = 0;
    read_bytes(cb, &read_buf, &read_pos, &ncount, sizeof(ncount));
    assert(ncount <= *inout_count);
    *inout_count = ncount;

    size_t total = 0;
    for (uint32_t i = 0; i < ncount; i++) {
        uint32_t sz = 0;
        read_bytes(cb, &read_buf, &read_pos, &sz, sizeof(sz));
        bufs[i].size = sz;
        size_t need = total + sz;
        if (ensure_scratch(cb, need) != 0)
            return 0;
        read_bytes(cb, &read_buf, &read_pos, cb->scratch + total, sz);
        total = need;
    }
    for (uint32_t i = 0, off = 0; i < ncount; off += bufs[i].size, i++)
        bufs[i].data = cb->scratch + off;

    atomic_store_explicit(&read_buf->read_pos, read_pos, memory_order_release);
    atomic_store_explicit(&cb->read_buf, read_buf, memory_order_release);
    atomic_fetch_add_explicit(&cb->pop_count, 1, memory_order_relaxed);
    return 1;
}
