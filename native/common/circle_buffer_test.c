#include "circle_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail = 1; \
    } \
} while (0)

static void test_create_destroy(void) {
    CHECK(circle_buffer_create(8, 0) == NULL); /* too small for header+payload */
    circle_buffer *cb = circle_buffer_create(64, 0);
    CHECK(cb != NULL);
    circle_buffer_destroy(cb);
    circle_buffer_destroy(NULL); /* must be safe */
}

static void test_raw_roundtrip(void) {
    circle_buffer *cb = circle_buffer_create(64, 0);
    CHECK(cb != NULL);

    CHECK(circle_buffer_can_pop(cb) == 0);
    CHECK(circle_buffer_waiting_count(cb) == 0);

    const char *msg = "hello-circle";
    CHECK(circle_buffer_push_raw_one(cb, msg, (uint32_t)strlen(msg)) == 0);
    CHECK(circle_buffer_waiting_count(cb) == 1);
    CHECK(circle_buffer_can_pop(cb) == 1);

    char out[64];
    memset(out, 0, sizeof(out));
    uint32_t n = circle_buffer_pop_raw(cb, out, sizeof(out));
    CHECK(n == (uint32_t)strlen(msg));
    CHECK(memcmp(out, msg, n) == 0);
    CHECK(circle_buffer_waiting_count(cb) == 0);
    CHECK(circle_buffer_can_pop(cb) == 0);

    /* force multi-block: payload_cap for block_size=64 is < 64 */
    uint8_t big[200];
    for (int i = 0; i < 200; i++)
        big[i] = (uint8_t)(i & 0xff);
    CHECK(circle_buffer_push_raw_one(cb, big, 200) == 0);
    uint8_t big_out[200];
    memset(big_out, 0, sizeof(big_out));
    n = circle_buffer_pop_raw(cb, big_out, 200);
    CHECK(n == 200);
    CHECK(memcmp(big_out, big, 200) == 0);

    circle_buf parts[2] = {
        { "AB", 2 },
        { "CD", 2 },
    };
    CHECK(circle_buffer_push_raw(cb, parts, 2) == 0);
    char four[8] = {0};
    n = circle_buffer_pop_raw(cb, four, 4);
    CHECK(n == 4);
    CHECK(memcmp(four, "ABCD", 4) == 0);

    circle_buffer_destroy(cb);
}

static void test_framed(void) {
    circle_buffer *cb = circle_buffer_create(128, 0);
    CHECK(cb != NULL);

    uint32_t ctx_in = 0x12345678u;
    circle_buf parts[2] = { { "hello", 5 }, { "world", 5 } };
    CHECK(circle_buffer_push_buffer(cb, &ctx_in, sizeof(ctx_in), parts, 2, 0) == 0);

    uint32_t ctx_out = 0;
    circle_buf out[2];
    uint32_t count = 2;
    CHECK(circle_buffer_pop_buffer(cb, &ctx_out, sizeof(ctx_out), out, &count) == 1);
    CHECK(ctx_out == ctx_in);
    CHECK(count == 2);
    CHECK(out[0].size == 5 && memcmp(out[0].data, "hello", 5) == 0);
    CHECK(out[1].size == 5 && memcmp(out[1].data, "world", 5) == 0);

    /* merge: one segment, concatenated bytes */
    CHECK(circle_buffer_push_buffer(cb, &ctx_in, sizeof(ctx_in), parts, 2, 1) == 0);
    count = 1;
    CHECK(circle_buffer_pop_buffer(cb, &ctx_out, sizeof(ctx_out), out, &count) == 1);
    CHECK(count == 1);
    CHECK(out[0].size == 10);
    CHECK(memcmp(out[0].data, "helloworld", 10) == 0);

    circle_buffer_destroy(cb);
}

static void test_free_empty_smoke(void) {
    circle_buffer *cb = circle_buffer_create(64, 1);
    CHECK(cb != NULL);
    uint8_t chunk[40];
    memset(chunk, 0xab, sizeof(chunk));
    for (int i = 0; i < 50; i++)
        CHECK(circle_buffer_push_raw_one(cb, chunk, sizeof(chunk)) == 0);
    uint8_t out[40];
    while (circle_buffer_can_pop(cb)) {
        uint32_t n = circle_buffer_pop_raw(cb, out, sizeof(out));
        CHECK(n > 0);
    }
    /* push again after drain — reclaim path exercised inside write_bytes */
    for (int i = 0; i < 20; i++)
        CHECK(circle_buffer_push_raw_one(cb, chunk, sizeof(chunk)) == 0);
    while (circle_buffer_can_pop(cb))
        (void)circle_buffer_pop_raw(cb, out, sizeof(out));
    circle_buffer_destroy(cb);
}

int main(void) {
    test_create_destroy();
    test_raw_roundtrip();
    test_framed();
    test_free_empty_smoke();
    if (g_fail) {
        fprintf(stderr, "circle_buffer_test FAILED\n");
        return 1;
    }
    printf("circle_buffer_test OK\n");
    return 0;
}
