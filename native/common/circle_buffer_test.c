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

int main(void) {
    test_create_destroy();
    if (g_fail) {
        fprintf(stderr, "circle_buffer_test FAILED\n");
        return 1;
    }
    printf("circle_buffer_test OK\n");
    return 0;
}
