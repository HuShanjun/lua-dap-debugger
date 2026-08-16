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

int main(void) {
    test_create_destroy();
    test_raw_roundtrip();
    if (g_fail) {
        fprintf(stderr, "circle_buffer_test FAILED\n");
        return 1;
    }
    printf("circle_buffer_test OK\n");
    return 0;
}
