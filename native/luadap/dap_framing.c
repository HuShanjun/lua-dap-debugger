#include "dap_framing.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void dap_recv_buf_init(dap_recv_buf *b) {
    if (!b) return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void dap_recv_buf_free(dap_recv_buf *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int dap_recv_buf_append(dap_recv_buf *b, const void *p, size_t n) {
    size_t need;
    size_t cap;
    char *nd;

    if (!b) return -1;
    if (n == 0) return 0;
    if (!p) return -1;

    need = b->len + n + 1; /* extra byte for a convenience NUL */
    if (need < b->len) return -1; /* overflow */
    cap = b->cap;
    if (cap < need) {
        cap = cap ? cap : 256;
        while (cap < need) {
            if (cap > ((size_t)-1) / 2) return -1;
            cap *= 2;
        }
        nd = (char *)realloc(b->data, cap);
        if (!nd) return -1;
        b->data = nd;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int ci_ncmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
    }
    return 0;
}

/* 0 = parsed into *out_len, -1 = missing or invalid Content-Length. */
static int header_content_length(const char *header, size_t hlen, size_t *out_len) {
    size_t i = 0;
    static const char key[] = "content-length:";
    static const size_t klen = sizeof(key) - 1;

    while (i < hlen) {
        size_t start = i;
        size_t line_len;
        const char *line;
        const char *p;
        const char *end;
        char *endp;
        unsigned long v;

        while (i < hlen && header[i] != '\r' && header[i] != '\n') i++;
        line_len = i - start;
        if (i < hlen && header[i] == '\r') i++;
        if (i < hlen && header[i] == '\n') i++;

        if (line_len < klen) continue;
        line = header + start;
        if (ci_ncmp(line, key, klen) != 0) continue;

        p = line + klen;
        end = line + line_len;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end || *p < '0' || *p > '9') return -1;
        v = strtoul(p, &endp, 10);
        if (endp == p) return -1;
        *out_len = (size_t)v;
        return 0;
    }
    return -1;
}

int dap_try_parse_frame(dap_recv_buf *b, char **out_json, size_t *out_len) {
    const char *sep;
    size_t i;
    size_t header_len;
    size_t content_len = 0;
    size_t body_start;
    size_t rest;
    char *json;

    if (out_json) *out_json = NULL;
    if (out_len) *out_len = 0;
    if (!b || !b->data || b->len < 4) return 0;

    sep = NULL;
    for (i = 0; i + 3 < b->len; i++) {
        if (b->data[i] == '\r' && b->data[i + 1] == '\n' &&
            b->data[i + 2] == '\r' && b->data[i + 3] == '\n') {
            sep = b->data + i;
            break;
        }
    }
    if (!sep) return 0;

    header_len = (size_t)(sep - b->data);
    if (header_content_length(b->data, header_len, &content_len) != 0)
        return -1;

    body_start = header_len + 4;
    if (content_len > ((size_t)-1) - body_start) return -1;
    if (b->len < body_start + content_len) return 0; /* incomplete body */

    json = (char *)malloc(content_len + 1);
    if (!json) return -1;
    memcpy(json, b->data + body_start, content_len);
    json[content_len] = '\0';

    rest = b->len - (body_start + content_len);
    if (rest > 0)
        memmove(b->data, b->data + body_start + content_len, rest);
    b->len = rest;
    if (b->data) b->data[b->len] = '\0';

    if (out_json) *out_json = json;
    else free(json);
    if (out_len) *out_len = content_len;
    return 1;
}
