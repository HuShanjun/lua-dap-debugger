#ifndef DAP_FRAMING_H
#define DAP_FRAMING_H

#include <stddef.h>

typedef struct dap_recv_buf {
    char *data;
    size_t len;
    size_t cap;
} dap_recv_buf;

void dap_recv_buf_init(dap_recv_buf *b);
void dap_recv_buf_free(dap_recv_buf *b);
int dap_recv_buf_append(dap_recv_buf *b, const void *p, size_t n); /* 0 ok, -1 OOM */
/* Returns 1 if a full frame JSON body was extracted into *out_json (caller frees),
   0 if incomplete, -1 on fatal framing error. */
int dap_try_parse_frame(dap_recv_buf *b, char **out_json, size_t *out_len);

#endif
