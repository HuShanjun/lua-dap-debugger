/*
 * Multi-connection poll engine: listen/accept, outbound connect, send/recv.
 * This file must not call any Lua API.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif

#include "poll_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>

typedef SOCKET as_fd;
#define AS_INVALID_FD INVALID_SOCKET
typedef CRITICAL_SECTION as_mutex;
typedef WSAPOLLFD as_pollfd;

#define as_close_fd(fd) closesocket(fd)
#define as_poll(fds, n, ms) WSAPoll((fds), (n), (ms))
#define as_mutex_init(m) InitializeCriticalSection(m)
#define as_mutex_lock(m) EnterCriticalSection(m)
#define as_mutex_unlock(m) LeaveCriticalSection(m)
#define as_mutex_destroy(m) DeleteCriticalSection(m)
#define AS_WOULDBLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#define AS_INPROGRESS() \
    (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef int as_fd;
#define AS_INVALID_FD (-1)
typedef pthread_mutex_t as_mutex;
typedef struct pollfd as_pollfd;

#define as_close_fd(fd) close(fd)
#define as_poll(fds, n, ms) poll((fds), (n), (ms))
#define as_mutex_init(m) pthread_mutex_init(m, NULL)
#define as_mutex_lock(m) pthread_mutex_lock(m)
#define as_mutex_unlock(m) pthread_mutex_unlock(m)
#define as_mutex_destroy(m) pthread_mutex_destroy(m)
#define AS_WOULDBLOCK() (errno == EAGAIN || errno == EWOULDBLOCK)
#define AS_INPROGRESS() (errno == EINPROGRESS || errno == EWOULDBLOCK)
#endif

#ifndef POLLIN
#define POLLIN 0x0100
#endif
#ifndef POLLOUT
#define POLLOUT 0x0010
#endif
#ifndef POLLERR
#define POLLERR 0x0001
#endif
#ifndef POLLHUP
#define POLLHUP 0x0002
#endif

#define AS_SETERR(buf, buflen, ...) \
    do { \
        if ((buf) && (buflen) > 0) { \
            snprintf((buf), (buflen), __VA_ARGS__); \
        } \
    } while (0)

#define AS_KIND_INBOUND 0
#define AS_KIND_OUTBOUND 1
#define AS_KIND_CONNECTING 2

typedef struct as_conn {
    int id;
    as_fd fd;
    int kind;
    int closing;
    int close_emitted;
    int poll_idx;
    char *send_buf;
    size_t send_len;
    size_t send_off;
    size_t send_cap;
} as_conn;

typedef struct as_engine {
    as_mutex lock;
    int inited;

    as_fd listen_fd;
    as_fd wakeup_rd;
    as_fd wakeup_wr;
    int listen_close_requested;

    as_conn *conns;
    size_t conn_count;
    size_t conn_cap;
    int next_id;

    as_event *q;
    size_t q_count;
    size_t q_cap;

    as_pollfd *pfds;
    int pfds_cap;

#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    volatile int running;
    int thread_started;
} as_engine;

static as_engine g_eng;

int as_net_init(void) {
#ifdef _WIN32
    static int done = 0;
    WSADATA wsa;
    if (done) {
        return 0;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return -1;
    }
    done = 1;
#endif
    return 0;
}

static void engine_init_once(void) {
    if (g_eng.inited) {
        return;
    }
    memset(&g_eng, 0, sizeof(g_eng));
    g_eng.listen_fd = AS_INVALID_FD;
    g_eng.wakeup_rd = AS_INVALID_FD;
    g_eng.wakeup_wr = AS_INVALID_FD;
    g_eng.next_id = 1;
    as_mutex_init(&g_eng.lock);
    g_eng.inited = 1;
}

static int set_nonblock(as_fd fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void set_nodelay(as_fd fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof(yes));
}

static void close_fd_slot(as_fd *fd) {
    if (fd && *fd != AS_INVALID_FD) {
        as_close_fd(*fd);
        *fd = AS_INVALID_FD;
    }
}

static void engine_wakeup(void) {
    char b = 1;
    if (g_eng.wakeup_wr == AS_INVALID_FD) {
        return;
    }
    send(g_eng.wakeup_wr, &b, 1, 0);
}

static int enqueue_unlocked(as_event_type type, int conn_id, const char *p, size_t n) {
    as_event *ni;
    as_event *e;
    size_t cap;

    if (g_eng.q_count == g_eng.q_cap) {
        cap = g_eng.q_cap ? g_eng.q_cap * 2 : 8;
        ni = (as_event *)realloc(g_eng.q, cap * sizeof(as_event));
        if (!ni) {
            return -1;
        }
        g_eng.q = ni;
        g_eng.q_cap = cap;
    }
    e = &g_eng.q[g_eng.q_count];
    e->type = type;
    e->conn_id = conn_id;
    e->payload = NULL;
    e->len = 0;
    if (p && n > 0) {
        e->payload = (char *)malloc(n);
        if (!e->payload) {
            return -1;
        }
        memcpy(e->payload, p, n);
        e->len = n;
    }
    g_eng.q_count++;
    return 0;
}

static int alloc_conn_id_unlocked(void) {
    int id = g_eng.next_id;
    if (id <= 0) {
        id = 1;
    }
    g_eng.next_id = id + 1;
    if (g_eng.next_id <= 0) {
        g_eng.next_id = 1;
    }
    return id;
}

static as_conn *alloc_conn_unlocked(as_fd fd, int kind) {
    as_conn *c;
    size_t cap;
    as_conn *ni;

    if (g_eng.conn_count == g_eng.conn_cap) {
        cap = g_eng.conn_cap ? g_eng.conn_cap * 2 : 8;
        ni = (as_conn *)realloc(g_eng.conns, cap * sizeof(as_conn));
        if (!ni) {
            return NULL;
        }
        g_eng.conns = ni;
        g_eng.conn_cap = cap;
    }
    c = &g_eng.conns[g_eng.conn_count++];
    memset(c, 0, sizeof(*c));
    c->id = alloc_conn_id_unlocked();
    c->fd = fd;
    c->kind = kind;
    c->poll_idx = -1;
    return c;
}

static as_conn *conn_by_id_unlocked(int id) {
    size_t i;
    if (id <= 0) {
        return NULL;
    }
    for (i = 0; i < g_eng.conn_count; i++) {
        if (g_eng.conns[i].id == id) {
            return &g_eng.conns[i];
        }
    }
    return NULL;
}

static void conn_free_send(as_conn *c) {
    free(c->send_buf);
    c->send_buf = NULL;
    c->send_len = 0;
    c->send_off = 0;
    c->send_cap = 0;
}

static void conn_remove_at_unlocked(size_t i) {
    as_conn *c;
    if (i >= g_eng.conn_count) {
        return;
    }
    c = &g_eng.conns[i];
    if (c->fd != AS_INVALID_FD) {
        as_close_fd(c->fd);
        c->fd = AS_INVALID_FD;
    }
    conn_free_send(c);
    if (i + 1 < g_eng.conn_count) {
        g_eng.conns[i] = g_eng.conns[g_eng.conn_count - 1];
    }
    memset(&g_eng.conns[g_eng.conn_count - 1], 0, sizeof(as_conn));
    g_eng.conn_count--;
}

static void emit_close_unlocked(as_conn *c) {
    /* Poll thread or post-join only: never closesocket while WSAPoll holds fd. */
    if (c->fd != AS_INVALID_FD) {
        as_close_fd(c->fd);
        c->fd = AS_INVALID_FD;
    }
    c->closing = 0;
    conn_free_send(c);
    if (!c->close_emitted) {
        c->close_emitted = 1;
        enqueue_unlocked(AS_EVT_CLOSE, c->id, NULL, 0);
    }
}

static void request_close_unlocked(as_conn *c) {
    c->closing = 1;
    conn_free_send(c);
    if (!c->close_emitted) {
        c->close_emitted = 1;
        enqueue_unlocked(AS_EVT_CLOSE, c->id, NULL, 0);
    }
}

static int send_buf_append(as_conn *c, const char *p, size_t n) {
    size_t need;
    size_t cap;
    char *nb;

    if (c->send_off > 0) {
        if (c->send_len > c->send_off) {
            memmove(c->send_buf, c->send_buf + c->send_off, c->send_len - c->send_off);
            c->send_len -= c->send_off;
        } else {
            c->send_len = 0;
        }
        c->send_off = 0;
    }
    need = c->send_len + n;
    if (need > c->send_cap) {
        cap = c->send_cap ? c->send_cap * 2 : 256;
        while (cap < need) {
            cap *= 2;
        }
        nb = (char *)realloc(c->send_buf, cap);
        if (!nb) {
            return -1;
        }
        c->send_buf = nb;
        c->send_cap = cap;
    }
    memcpy(c->send_buf + c->send_len, p, n);
    c->send_len += n;
    return 0;
}

static void flush_send_unlocked(as_conn *c) {
    while (c->fd != AS_INVALID_FD && c->send_off < c->send_len) {
        int n = send(
            c->fd,
            c->send_buf + c->send_off,
            (int)(c->send_len - c->send_off),
            0);
        if (n > 0) {
            c->send_off += (size_t)n;
            if (c->send_off >= c->send_len) {
                c->send_len = 0;
                c->send_off = 0;
            }
            continue;
        }
        if (n < 0 && AS_WOULDBLOCK()) {
            break;
        }
        emit_close_unlocked(c);
        break;
    }
}

static void drain_wakeup(void) {
    char buf[64];
    while (g_eng.wakeup_rd != AS_INVALID_FD) {
        int n = recv(g_eng.wakeup_rd, buf, (int)sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
    }
}

static void apply_listen_close_unlocked(void) {
    if (g_eng.listen_close_requested) {
        close_fd_slot(&g_eng.listen_fd);
        g_eng.listen_close_requested = 0;
    }
}

static void handle_accept(void) {
    as_fd cfd;
    as_conn *c;

    if (g_eng.listen_fd == AS_INVALID_FD) {
        return;
    }
    cfd = accept(g_eng.listen_fd, NULL, NULL);
    if (cfd == AS_INVALID_FD) {
        return;
    }
    if (set_nonblock(cfd) != 0) {
        as_close_fd(cfd);
        return;
    }
    set_nodelay(cfd);
    c = alloc_conn_unlocked(cfd, AS_KIND_INBOUND);
    if (!c) {
        as_close_fd(cfd);
        return;
    }
    enqueue_unlocked(AS_EVT_ACCEPT, c->id, NULL, 0);
}

static void handle_conn_read(as_conn *c) {
    char buf[4096];
    for (;;) {
        int n;
        if (c->fd == AS_INVALID_FD) {
            return;
        }
        n = recv(c->fd, buf, (int)sizeof(buf), 0);
        if (n > 0) {
            enqueue_unlocked(AS_EVT_MESSAGE, c->id, buf, (size_t)n);
            continue;
        }
        if (n < 0 && AS_WOULDBLOCK()) {
            return;
        }
        emit_close_unlocked(c);
        return;
    }
}

static int sock_error(as_fd fd) {
    int soerr = 0;
#ifdef _WIN32
    int slen = (int)sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen) != 0) {
        return WSAGetLastError();
    }
#else
    socklen_t slen = (socklen_t)sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0) {
        return errno;
    }
#endif
    return soerr;
}

static void handle_connect_complete(as_conn *c, short rev) {
    int soerr;

    if (c->fd == AS_INVALID_FD) {
        return;
    }
    if (rev & (POLLERR | POLLHUP)) {
        emit_close_unlocked(c);
        return;
    }
    if (!(rev & POLLOUT)) {
        return;
    }
    soerr = sock_error(c->fd);
    if (soerr != 0) {
        emit_close_unlocked(c);
        return;
    }
    set_nodelay(c->fd);
    c->kind = AS_KIND_OUTBOUND;
    enqueue_unlocked(AS_EVT_OPEN, c->id, NULL, 0);
}

static int pfds_ensure(int needed) {
    as_pollfd *ni;
    int cap;

    if (needed <= g_eng.pfds_cap) {
        return 0;
    }
    cap = g_eng.pfds_cap ? g_eng.pfds_cap * 2 : 8;
    while (cap < needed) {
        cap *= 2;
    }
    ni = (as_pollfd *)realloc(g_eng.pfds, (size_t)cap * sizeof(as_pollfd));
    if (!ni) {
        return -1;
    }
    g_eng.pfds = ni;
    g_eng.pfds_cap = cap;
    return 0;
}

static void poll_loop(void) {
    while (g_eng.running) {
        as_pollfd *fds;
        int nfds = 0;
        int i_listen = -1;
        int i_wake = -1;
        int pr;
        size_t i;

        as_mutex_lock(&g_eng.lock);
        apply_listen_close_unlocked();
        for (i = 0; i < g_eng.conn_count;) {
            as_conn *c = &g_eng.conns[i];
            if (c->closing) {
                emit_close_unlocked(c);
                conn_remove_at_unlocked(i);
                continue;
            }
            i++;
        }

        if (pfds_ensure((int)g_eng.conn_count + 2) != 0) {
            as_mutex_unlock(&g_eng.lock);
#ifdef _WIN32
            Sleep(1);
#else
            usleep(1000);
#endif
            continue;
        }
        fds = g_eng.pfds;

        if (g_eng.listen_fd != AS_INVALID_FD) {
            i_listen = nfds;
            fds[nfds].fd = g_eng.listen_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        for (i = 0; i < g_eng.conn_count; i++) {
            as_conn *c = &g_eng.conns[i];
            c->poll_idx = nfds;
            fds[nfds].fd = c->fd;
            if (c->kind == AS_KIND_CONNECTING) {
                fds[nfds].events = POLLOUT;
            } else {
                fds[nfds].events = POLLIN;
                if (c->send_off < c->send_len) {
                    fds[nfds].events = (short)(fds[nfds].events | POLLOUT);
                }
            }
            fds[nfds].revents = 0;
            nfds++;
        }
        if (g_eng.wakeup_rd != AS_INVALID_FD) {
            i_wake = nfds;
            fds[nfds].fd = g_eng.wakeup_rd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        as_mutex_unlock(&g_eng.lock);

        if (nfds == 0) {
            break;
        }

        pr = as_poll(fds, (unsigned long)nfds, 100);
        if (!g_eng.running) {
            break;
        }
        if (pr < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
#else
            if (errno == EINTR) {
                continue;
            }
#endif
            continue;
        }

        as_mutex_lock(&g_eng.lock);
        apply_listen_close_unlocked();
        if (i_wake >= 0 && (fds[i_wake].revents & POLLIN)) {
            drain_wakeup();
        }
        if (i_listen >= 0 && g_eng.listen_fd != AS_INVALID_FD &&
            (fds[i_listen].revents & POLLIN)) {
            handle_accept();
        }
        for (i = 0; i < g_eng.conn_count;) {
            as_conn *c = &g_eng.conns[i];
            short rev = 0;
            int pidx = c->poll_idx;

            if (c->closing) {
                emit_close_unlocked(c);
                conn_remove_at_unlocked(i);
                continue;
            }
            if (pidx >= 0 && pidx < nfds) {
                rev = fds[pidx].revents;
            }
            if (c->kind == AS_KIND_CONNECTING) {
                if (rev & (POLLOUT | POLLERR | POLLHUP)) {
                    handle_connect_complete(c, rev);
                }
                if (c->fd == AS_INVALID_FD) {
                    conn_remove_at_unlocked(i);
                    continue;
                }
                i++;
                continue;
            }
            if (rev & (POLLIN | POLLHUP | POLLERR)) {
                handle_conn_read(c);
            }
            if (c->fd != AS_INVALID_FD && (rev & POLLOUT)) {
                flush_send_unlocked(c);
            }
            if (c->fd == AS_INVALID_FD) {
                conn_remove_at_unlocked(i);
                continue;
            }
            i++;
        }
        as_mutex_unlock(&g_eng.lock);
    }
}

#ifdef _WIN32
static unsigned __stdcall poll_thread_main(void *arg) {
    (void)arg;
    poll_loop();
    return 0;
}
#else
static void *poll_thread_main(void *arg) {
    (void)arg;
    poll_loop();
    return NULL;
}
#endif

static int create_wakeup_pair(as_fd *rd, as_fd *wr, char *err, size_t errlen) {
    as_fd listener = AS_INVALID_FD;
    as_fd connector = AS_INVALID_FD;
    as_fd accepted = AS_INVALID_FD;
    struct sockaddr_in addr;
    socklen_t alen = (socklen_t)sizeof(addr);
    int yes = 1;

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == AS_INVALID_FD) {
        AS_SETERR(err, errlen, "wakeup socket() failed");
        return -1;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 1) != 0) {
        AS_SETERR(err, errlen, "wakeup bind/listen failed");
        as_close_fd(listener);
        return -1;
    }
    if (getsockname(listener, (struct sockaddr *)&addr, &alen) != 0) {
        AS_SETERR(err, errlen, "wakeup getsockname failed");
        as_close_fd(listener);
        return -1;
    }
    connector = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connector == AS_INVALID_FD) {
        AS_SETERR(err, errlen, "wakeup connector socket() failed");
        as_close_fd(listener);
        return -1;
    }
    if (connect(connector, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        AS_SETERR(err, errlen, "wakeup connect failed");
        as_close_fd(connector);
        as_close_fd(listener);
        return -1;
    }
    accepted = accept(listener, NULL, NULL);
    as_close_fd(listener);
    if (accepted == AS_INVALID_FD) {
        AS_SETERR(err, errlen, "wakeup accept failed");
        as_close_fd(connector);
        return -1;
    }
    if (set_nonblock(accepted) != 0 || set_nonblock(connector) != 0) {
        AS_SETERR(err, errlen, "wakeup set_nonblock failed");
        as_close_fd(accepted);
        as_close_fd(connector);
        return -1;
    }
    set_nodelay(accepted);
    set_nodelay(connector);
    *rd = accepted;
    *wr = connector;
    return 0;
}

static as_fd create_listen_fd(const char *host, int port, char *err, size_t errlen) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    as_fd fd = AS_INVALID_FD;
    char portstr[16];
    int rc;
    int yes = 1;
    const char *node = host;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (!node || !node[0] || strcmp(node, "*") == 0 || strcmp(node, "0.0.0.0") == 0) {
        hints.ai_flags = AI_PASSIVE;
        node = NULL;
    }
    snprintf(portstr, sizeof(portstr), "%d", port);
    rc = getaddrinfo(node, portstr, &hints, &res);
    if (rc != 0) {
#ifdef _WIN32
        AS_SETERR(err, errlen, "getaddrinfo failed (%d)", WSAGetLastError());
#else
        AS_SETERR(err, errlen, "getaddrinfo failed: %s", gai_strerror(rc));
#endif
        return AS_INVALID_FD;
    }
    for (it = res; it; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == AS_INVALID_FD) {
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
        if (bind(fd, it->ai_addr, (int)it->ai_addrlen) != 0) {
            as_close_fd(fd);
            fd = AS_INVALID_FD;
            continue;
        }
        if (listen(fd, SOMAXCONN) != 0) {
            as_close_fd(fd);
            fd = AS_INVALID_FD;
            continue;
        }
        if (set_nonblock(fd) != 0) {
            as_close_fd(fd);
            fd = AS_INVALID_FD;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd == AS_INVALID_FD) {
        AS_SETERR(err, errlen, "bind/listen failed on %s:%d", host ? host : "*", port);
    }
    return fd;
}

static int begin_connect_fd(
    const char *host,
    int port,
    as_fd *out_fd,
    int *out_immediate,
    char *err,
    size_t errlen) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    char portstr[16];
    int rc;
    const char *node = host;

    *out_fd = AS_INVALID_FD;
    *out_immediate = 0;
    if (!node || !node[0]) {
        AS_SETERR(err, errlen, "invalid host");
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(portstr, sizeof(portstr), "%d", port);
    rc = getaddrinfo(node, portstr, &hints, &res);
    if (rc != 0) {
#ifdef _WIN32
        AS_SETERR(err, errlen, "getaddrinfo failed (%d)", WSAGetLastError());
#else
        AS_SETERR(err, errlen, "getaddrinfo failed: %s", gai_strerror(rc));
#endif
        return -1;
    }
    for (it = res; it; it = it->ai_next) {
        as_fd fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == AS_INVALID_FD) {
            continue;
        }
        if (set_nonblock(fd) != 0) {
            as_close_fd(fd);
            continue;
        }
        rc = connect(fd, it->ai_addr, (int)it->ai_addrlen);
        if (rc == 0) {
            set_nodelay(fd);
            *out_fd = fd;
            *out_immediate = 1;
            freeaddrinfo(res);
            return 0;
        }
        if (AS_INPROGRESS()) {
            *out_fd = fd;
            *out_immediate = 0;
            freeaddrinfo(res);
            return 0;
        }
        as_close_fd(fd);
    }
    freeaddrinfo(res);
    AS_SETERR(err, errlen, "connect failed to %s:%d", host, port);
    return -1;
}

static int engine_ensure_running(char *err, size_t errlen) {
    if (g_eng.thread_started) {
        return 0;
    }
    if (create_wakeup_pair(&g_eng.wakeup_rd, &g_eng.wakeup_wr, err, errlen) != 0) {
        return -1;
    }
    g_eng.running = 1;
#ifdef _WIN32
    g_eng.thread = (HANDLE)_beginthreadex(NULL, 0, poll_thread_main, NULL, 0, NULL);
    if (!g_eng.thread) {
        AS_SETERR(err, errlen, "failed to start poll thread");
        g_eng.running = 0;
        close_fd_slot(&g_eng.wakeup_rd);
        close_fd_slot(&g_eng.wakeup_wr);
        return -1;
    }
#else
    if (pthread_create(&g_eng.thread, NULL, poll_thread_main, NULL) != 0) {
        AS_SETERR(err, errlen, "failed to start poll thread");
        g_eng.running = 0;
        close_fd_slot(&g_eng.wakeup_rd);
        close_fd_slot(&g_eng.wakeup_wr);
        return -1;
    }
#endif
    g_eng.thread_started = 1;
    return 0;
}

static void join_thread(void) {
    if (!g_eng.thread_started) {
        return;
    }
#ifdef _WIN32
    if (g_eng.thread) {
        WaitForSingleObject(g_eng.thread, INFINITE);
        CloseHandle(g_eng.thread);
        g_eng.thread = NULL;
    }
#else
    pthread_join(g_eng.thread, NULL);
#endif
    g_eng.thread_started = 0;
}

static void sleep_1ms(void) {
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
}

int as_listen(const char *host, int port, char *err, size_t errlen) {
    as_fd fd;
    int tries;

    if (as_net_init() != 0) {
        AS_SETERR(err, errlen, "WSAStartup failed");
        return -1;
    }
    engine_init_once();
    if (!host || port <= 0 || port > 65535) {
        AS_SETERR(err, errlen, "invalid host/port");
        return -1;
    }

    for (tries = 0; tries < 1000; tries++) {
        as_mutex_lock(&g_eng.lock);
        if (g_eng.listen_close_requested) {
            as_mutex_unlock(&g_eng.lock);
            engine_wakeup();
            sleep_1ms();
            continue;
        }
        if (g_eng.listen_fd != AS_INVALID_FD) {
            as_mutex_unlock(&g_eng.lock);
            AS_SETERR(err, errlen, "listen already active");
            return -1;
        }
        as_mutex_unlock(&g_eng.lock);
        break;
    }

    fd = create_listen_fd(host, port, err, errlen);
    if (fd == AS_INVALID_FD) {
        return -1;
    }

    as_mutex_lock(&g_eng.lock);
    if (g_eng.listen_fd != AS_INVALID_FD || g_eng.listen_close_requested) {
        as_mutex_unlock(&g_eng.lock);
        as_close_fd(fd);
        AS_SETERR(err, errlen, "listen already active");
        return -1;
    }
    if (engine_ensure_running(err, errlen) != 0) {
        as_mutex_unlock(&g_eng.lock);
        as_close_fd(fd);
        return -1;
    }
    g_eng.listen_fd = fd;
    as_mutex_unlock(&g_eng.lock);
    engine_wakeup();
    return 0;
}

int as_connect(const char *host, int port, char *err, size_t errlen) {
    as_fd fd;
    int immediate = 0;
    as_conn *c;
    int id;

    if (as_net_init() != 0) {
        AS_SETERR(err, errlen, "WSAStartup failed");
        return -1;
    }
    engine_init_once();
    if (!host || port <= 0 || port > 65535) {
        AS_SETERR(err, errlen, "invalid host/port");
        return -1;
    }
    if (begin_connect_fd(host, port, &fd, &immediate, err, errlen) != 0) {
        return -1;
    }

    as_mutex_lock(&g_eng.lock);
    if (engine_ensure_running(err, errlen) != 0) {
        as_mutex_unlock(&g_eng.lock);
        as_close_fd(fd);
        return -1;
    }
    c = alloc_conn_unlocked(fd, immediate ? AS_KIND_OUTBOUND : AS_KIND_CONNECTING);
    if (!c) {
        as_mutex_unlock(&g_eng.lock);
        as_close_fd(fd);
        AS_SETERR(err, errlen, "out of memory");
        return -1;
    }
    id = c->id;
    if (immediate) {
        enqueue_unlocked(AS_EVT_OPEN, id, NULL, 0);
    }
    as_mutex_unlock(&g_eng.lock);
    engine_wakeup();
    return id;
}

int as_conn_send(int conn_id, const void *data, size_t len) {
    const char *p = (const char *)data;
    as_conn *c;
    int need_pollout = 0;
    int fatal = 0;

    engine_init_once();
    if (conn_id <= 0 || !data) {
        return -1;
    }
    as_mutex_lock(&g_eng.lock);
    c = conn_by_id_unlocked(conn_id);
    if (!c || c->fd == AS_INVALID_FD || c->closing || c->kind == AS_KIND_CONNECTING) {
        as_mutex_unlock(&g_eng.lock);
        return -1;
    }
    if (c->send_off >= c->send_len && len > 0) {
        int n = send(c->fd, p, (int)len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
        } else if (n < 0 && !AS_WOULDBLOCK()) {
            request_close_unlocked(c);
            fatal = 1;
        }
    }
    if (!fatal && len > 0) {
        if (send_buf_append(c, p, len) != 0) {
            as_mutex_unlock(&g_eng.lock);
            return -1;
        }
        need_pollout = 1;
    }
    as_mutex_unlock(&g_eng.lock);
    if (need_pollout || fatal) {
        engine_wakeup();
    }
    return fatal ? -1 : 0;
}

void as_conn_close(int conn_id) {
    as_conn *c;

    engine_init_once();
    if (conn_id <= 0) {
        return;
    }
    as_mutex_lock(&g_eng.lock);
    c = conn_by_id_unlocked(conn_id);
    if (c && !c->closing) {
        request_close_unlocked(c);
    }
    as_mutex_unlock(&g_eng.lock);
    engine_wakeup();
}

void as_server_close(void) {
    engine_init_once();
    as_mutex_lock(&g_eng.lock);
    if (g_eng.listen_fd != AS_INVALID_FD) {
        g_eng.listen_close_requested = 1;
    }
    as_mutex_unlock(&g_eng.lock);
    engine_wakeup();
}

void as_engine_stop(void) {
    size_t i;

    engine_init_once();
    g_eng.running = 0;
    engine_wakeup();
    join_thread();

    as_mutex_lock(&g_eng.lock);
    for (i = 0; i < g_eng.conn_count; i++) {
        as_conn *c = &g_eng.conns[i];
        if (c->fd != AS_INVALID_FD && !c->close_emitted) {
            emit_close_unlocked(c);
        } else {
            close_fd_slot(&c->fd);
        }
        conn_free_send(c);
    }
    g_eng.conn_count = 0;
    g_eng.listen_close_requested = 0;
    close_fd_slot(&g_eng.listen_fd);
    close_fd_slot(&g_eng.wakeup_rd);
    close_fd_slot(&g_eng.wakeup_wr);
    as_mutex_unlock(&g_eng.lock);
}

as_event *as_take_events(size_t *out_n) {
    as_event *out;

    engine_init_once();
    if (!out_n) {
        return NULL;
    }
    as_mutex_lock(&g_eng.lock);
    out = g_eng.q;
    *out_n = g_eng.q_count;
    g_eng.q = NULL;
    g_eng.q_count = 0;
    g_eng.q_cap = 0;
    as_mutex_unlock(&g_eng.lock);
    return out;
}

void as_events_free(as_event *evs, size_t n) {
    size_t i;
    if (!evs) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(evs[i].payload);
    }
    free(evs);
}

/* ---- Task 1 compat wrappers (remove in Task 4) ---- */

struct as_socket {
    int stopped;
    int current_conn_id;
};

as_socket *as_socket_listen(const char *host, int port, char *err, size_t errlen) {
    as_socket *s;

    if (as_listen(host, port, err, errlen) != 0) {
        return NULL;
    }
    s = (as_socket *)calloc(1, sizeof(as_socket));
    if (!s) {
        as_server_close();
        as_engine_stop();
        AS_SETERR(err, errlen, "out of memory");
        return NULL;
    }
    return s;
}

void as_socket_wakeup(as_socket *s) {
    (void)s;
    engine_init_once();
    engine_wakeup();
}

void as_socket_stop(as_socket *s) {
    if (!s || s->stopped) {
        return;
    }
    as_server_close();
    as_engine_stop();
    s->stopped = 1;
    /* Keep current_conn_id so a subsequent take_events still delivers CLOSE. */
}

void as_socket_destroy(as_socket *s) {
    as_event *evs;
    size_t n = 0;

    if (!s) {
        return;
    }
    as_socket_stop(s);
    evs = as_take_events(&n);
    as_events_free(evs, n);
    free(s);
}

int as_socket_send(as_socket *s, const void *data, size_t len) {
    if (!s || s->stopped || s->current_conn_id <= 0) {
        return -1;
    }
    return as_conn_send(s->current_conn_id, data, len);
}

as_event *as_socket_take_events(as_socket *s, size_t *out_n) {
    as_event *raw;
    as_event *out;
    size_t n = 0;
    size_t i;
    size_t m = 0;

    if (!s || !out_n) {
        if (out_n) {
            *out_n = 0;
        }
        return NULL;
    }
    raw = as_take_events(&n);
    if (!raw || n == 0) {
        *out_n = 0;
        as_events_free(raw, n);
        return NULL;
    }
    out = (as_event *)calloc(n, sizeof(as_event));
    if (!out) {
        as_events_free(raw, n);
        *out_n = 0;
        return NULL;
    }
    for (i = 0; i < n; i++) {
        as_event *e = &raw[i];
        if (e->type == AS_EVT_ACCEPT) {
            if (s->current_conn_id <= 0) {
                s->current_conn_id = e->conn_id;
                out[m].type = AS_EVT_OPEN;
                out[m].conn_id = e->conn_id;
                out[m].payload = NULL;
                out[m].len = 0;
                m++;
            } else {
                as_conn_close(e->conn_id);
            }
            continue;
        }
        if (e->type == AS_EVT_OPEN) {
            /* Outbound; DAP compat ignores unless it is the tracked conn. */
            if (s->current_conn_id == e->conn_id) {
                out[m] = *e;
                m++;
            }
            continue;
        }
        if (s->current_conn_id > 0 && e->conn_id == s->current_conn_id) {
            out[m] = *e;
            e->payload = NULL;
            m++;
            if (e->type == AS_EVT_CLOSE) {
                s->current_conn_id = 0;
            }
        }
    }
    as_events_free(raw, n);
    if (m == 0) {
        free(out);
        *out_n = 0;
        return NULL;
    }
    *out_n = m;
    return out;
}
