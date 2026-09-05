#ifndef CIRCLE_BUFFER_H
#define CIRCLE_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct circle_buffer circle_buffer;

typedef struct circle_buf {
    const void *data;
    uint32_t size;
} circle_buf;

circle_buffer *circle_buffer_create(uint32_t block_size, int free_empty_buffer);
void circle_buffer_destroy(circle_buffer *cb);

int circle_buffer_push_raw(circle_buffer *cb, const circle_buf *bufs, uint32_t count);
int circle_buffer_push_raw_one(circle_buffer *cb, const void *data, uint32_t size);
uint32_t circle_buffer_pop_raw(circle_buffer *cb, void *out, uint32_t out_size);

int circle_buffer_push_buffer(circle_buffer *cb, const void *context, size_t context_size,
                              const circle_buf *bufs, uint32_t count, int merge);
int circle_buffer_pop_buffer(circle_buffer *cb, void *context_out, size_t context_size,
                             circle_buf *bufs, uint32_t *inout_count);

int circle_buffer_can_pop(const circle_buffer *cb);
uint32_t circle_buffer_waiting_count(const circle_buffer *cb);

#ifdef __cplusplus
}
#endif

#endif
