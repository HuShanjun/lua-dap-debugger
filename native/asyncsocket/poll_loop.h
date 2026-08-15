#ifndef ASYNCSOCKET_POLL_LOOP_H
#define ASYNCSOCKET_POLL_LOOP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AS_EVT_OPEN = 1,
    AS_EVT_MESSAGE = 2,
    AS_EVT_CLOSE = 3
} as_event_type;

typedef struct as_event {
    as_event_type type;
    char *payload;
    size_t len;
} as_event;

typedef struct as_socket as_socket;

int as_net_init(void);

as_socket *as_socket_listen(const char *host, int port, char *err, size_t errlen);
void as_socket_stop(as_socket *s);
void as_socket_destroy(as_socket *s);
void as_socket_wakeup(as_socket *s);
int as_socket_send(as_socket *s, const void *data, size_t len);

/* Steal queued events. Caller must as_events_free. Poll thread never calls Lua. */
as_event *as_socket_take_events(as_socket *s, size_t *out_n);
void as_events_free(as_event *evs, size_t n);

#ifdef __cplusplus
}
#endif

#endif
