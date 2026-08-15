#include "dap_session.h"
#include "poll_loop.h"
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static void short_sleep(void) { Sleep(1); }
#else
#include <time.h>
static void short_sleep(void) {
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
}
#endif

struct dap_session {
    as_socket *sock;
    int client_open;
    int configured;
    int dead;
    int close_pending;
    int hook_installed;
};

static dap_session g_sess;

dap_session *dap_session_get(void) { return &g_sess; }

int dap_session_start(lua_State *L, const char *host, int port, int wait) {
    (void)L;
    (void)wait;
    memset(&g_sess, 0, sizeof(g_sess));
    if (as_net_init() != 0) return -1;
    char err[256];
    g_sess.sock = as_socket_listen(host, port, err, sizeof(err));
    if (!g_sess.sock) {
        fprintf(stderr, "[luadap] listen failed: %s\n", err);
        return -1;
    }
    fprintf(stderr, "[luadap] listening on %s:%d\n", host, port);
    return 0;
}

int dap_session_update(lua_State *L) {
    (void)L;
    if (!g_sess.sock || g_sess.dead) return 0;
    size_t n = 0;
    as_event *evs = as_socket_take_events(g_sess.sock, &n);
    for (size_t i = 0; i < n; i++) {
        if (evs[i].type == AS_EVT_OPEN) g_sess.client_open = 1;
        if (evs[i].type == AS_EVT_CLOSE) g_sess.close_pending = 1;
    }
    as_events_free(evs, n);
    return 0;
}

void dap_session_shutdown(lua_State *L, cJSON *disconnect_req) {
    (void)L;
    (void)disconnect_req;
    if (g_sess.dead) return;
    g_sess.dead = 1;
    if (g_sess.sock) {
        as_socket_stop(g_sess.sock);
        as_socket_destroy(g_sess.sock);
        g_sess.sock = NULL;
    }
}
