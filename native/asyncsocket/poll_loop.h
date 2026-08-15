#ifndef ASYNCSOCKET_POLL_LOOP_H
#define ASYNCSOCKET_POLL_LOOP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Global V1 poll engine: one listen, many connections, one poll thread.
 * conn_id starts at 1 and is never 0.
 * Poll thread never calls Lua; callers drain via as_take_events on the
 * main thread.
 */

typedef enum {
    AS_EVT_ACCEPT = 1, /* inbound ready; conn_id set */
    AS_EVT_OPEN = 2,   /* outbound connect done */
    AS_EVT_MESSAGE = 3,
    AS_EVT_CLOSE = 4
} as_event_type;

typedef struct as_event {
    as_event_type type;
    int conn_id;   /* >0 */
    char *payload; /* MESSAGE only; else NULL */
    size_t len;
} as_event;

int as_net_init(void);

/* Global V1 engine (one listen max). err buffers optional. */
int as_listen(const char *host, int port, char *err, size_t errlen); /* 0 ok */
int as_connect(const char *host, int port, char *err, size_t errlen); /* conn_id>0 or -1 */
int as_conn_send(int conn_id, const void *data, size_t len); /* 0 ok, -1 fail */
void as_conn_close(int conn_id);
void as_server_close(void); /* listen only; existing conns remain */
void as_engine_stop(void);  /* stop poll thread, close all fds (process teardown) */

as_event *as_take_events(size_t *out_n);
void as_events_free(as_event *evs, size_t n);

/*
 * Temporary Task 1 compat for dap_session.c / asyncsocket.c (removed in Task 4).
 * Single-client semantics: ACCEPT → OPEN; extra inbound conns are closed.
 */
typedef struct as_socket as_socket;

as_socket *as_socket_listen(const char *host, int port, char *err, size_t errlen);
void as_socket_stop(as_socket *s);
void as_socket_destroy(as_socket *s);
void as_socket_wakeup(as_socket *s);
int as_socket_send(as_socket *s, const void *data, size_t len);
as_event *as_socket_take_events(as_socket *s, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif
