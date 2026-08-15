#include "dap_framing.h"

void dap_recv_buf_init(dap_recv_buf *b) {
    if (!b) return;
    b->data = 0;
    b->len = 0;
    b->cap = 0;
}

void dap_recv_buf_free(dap_recv_buf *b) {
    (void)b;
}

int dap_recv_buf_append(dap_recv_buf *b, const void *p, size_t n) {
    (void)b;
    (void)p;
    (void)n;
    return -1;
}

int dap_try_parse_frame(dap_recv_buf *b, char **out_json, size_t *out_len) {
    (void)b;
    if (out_json) *out_json = 0;
    if (out_len) *out_len = 0;
    return 0;
}
