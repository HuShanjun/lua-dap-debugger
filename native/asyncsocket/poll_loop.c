/*
 * Poll thread: WSAPoll/poll, accept/recv, enqueue events, flush send buf.
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

struct as_socket {
    as_fd listen_fd;
    as_fd client_fd;
    as_fd wakeup_rd;
    as_fd wakeup_wr;

    as_mutex lock;
    as_event *q;
    size_t q_count;
    size_t q_cap;

    char *send_buf;
    size_t send_len;
    size_t send_off;
    size_t send_cap;

#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    volatile int running;
    int stopped;
    int close_emitted;
    int thread_started;
};

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

static int enqueue_unlocked(as_socket *s, as_event_type type, const char *p, size_t n) {
    as_event *ni;
    as_event *e;
    size_t cap;

    if (s->q_count == s->q_cap) {
        cap = s->q_cap ? s->q_cap * 2 : 8;
        ni = (as_event *)realloc(s->q, cap * sizeof(as_event));
        if (!ni) {
            return -1;
        }
        s->q = ni;
        s->q_cap = cap;
    }
    e = &s->q[s->q_count];
    e->type = type;
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
    s->q_count++;
    return 0;
}

static void emit_close_unlocked(as_socket *s) {
    if (s->client_fd != AS_INVALID_FD) {
        as_close_fd(s->client_fd);
        s->client_fd = AS_INVALID_FD;
    }
    s->send_len = 0;
    s->send_off = 0;
    if (!s->close_emitted) {
        s->close_emitted = 1;
        enqueue_unlocked(s, AS_EVT_CLOSE, NULL, 0);
    }
}

static int send_buf_append(as_socket *s, const char *p, size_t n) {
    size_t need;
    size_t cap;
    char *nb;

    if (s->send_off > 0) {
        if (s->send_len > s->send_off) {
            memmove(s->send_buf, s->send_buf + s->send_off, s->send_len - s->send_off);
            s->send_len -= s->send_off;
        } else {
            s->send_len = 0;
        }
        s->send_off = 0;
    }
    need = s->send_len + n;
    if (need > s->send_cap) {
        cap = s->send_cap ? s->send_cap * 2 : 256;
        while (cap < need) {
            cap *= 2;
        }
        nb = (char *)realloc(s->send_buf, cap);
        if (!nb) {
            return -1;
        }
        s->send_buf = nb;
        s->send_cap = cap;
    }
    memcpy(s->send_buf + s->send_len, p, n);
    s->send_len += n;
    return 0;
}

static void flush_send_unlocked(as_socket *s) {
    while (s->client_fd != AS_INVALID_FD && s->send_off < s->send_len) {
        int n = send(
            s->client_fd,
            s->send_buf + s->send_off,
            (int)(s->send_len - s->send_off),
            0);
        if (n > 0) {
            s->send_off += (size_t)n;
            if (s->send_off >= s->send_len) {
                s->send_len = 0;
                s->send_off = 0;
            }
            continue;
        }
        if (n < 0 && AS_WOULDBLOCK()) {
            break;
        }
        emit_close_unlocked(s);
        break;
    }
}

static void drain_wakeup(as_socket *s) {
    char buf[64];
    while (s->wakeup_rd != AS_INVALID_FD) {
        int n = recv(s->wakeup_rd, buf, (int)sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
    }
}

static void handle_accept(as_socket *s) {
    as_fd c = accept(s->listen_fd, NULL, NULL);
    if (c == AS_INVALID_FD) {
        return;
    }
    if (set_nonblock(c) != 0) {
        as_close_fd(c);
        return;
    }
    set_nodelay(c);
    /*
     * V1: one listen + at most one client.
     * Choice: reject the new connection (close it) and keep the existing client.
     */
    if (s->client_fd != AS_INVALID_FD) {
        as_close_fd(c);
        return;
    }
    s->client_fd = c;
    s->close_emitted = 0;
    enqueue_unlocked(s, AS_EVT_OPEN, NULL, 0);
}

static void handle_client_read(as_socket *s) {
    char buf[4096];
    for (;;) {
        int n;
        if (s->client_fd == AS_INVALID_FD) {
            return;
        }
        n = recv(s->client_fd, buf, (int)sizeof(buf), 0);
        if (n > 0) {
            enqueue_unlocked(s, AS_EVT_MESSAGE, buf, (size_t)n);
            continue;
        }
        if (n < 0 && AS_WOULDBLOCK()) {
            return;
        }
        /* recv==0, fatal error, or hangup: peer closed / dead connection */
        emit_close_unlocked(s);
        return;
    }
}

static void poll_loop(as_socket *s) {
    while (s->running) {
        as_pollfd fds[3];
        int nfds = 0;
        int i_listen = -1;
        int i_client = -1;
        int i_wake = -1;
        int pr;

        as_mutex_lock(&s->lock);
        if (s->listen_fd != AS_INVALID_FD) {
            i_listen = nfds;
            fds[nfds].fd = s->listen_fd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        if (s->client_fd != AS_INVALID_FD) {
            i_client = nfds;
            fds[nfds].fd = s->client_fd;
            fds[nfds].events = POLLIN;
            if (s->send_off < s->send_len) {
                fds[nfds].events = (short)(fds[nfds].events | POLLOUT);
            }
            fds[nfds].revents = 0;
            nfds++;
        }
        if (s->wakeup_rd != AS_INVALID_FD) {
            i_wake = nfds;
            fds[nfds].fd = s->wakeup_rd;
            fds[nfds].events = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        as_mutex_unlock(&s->lock);

        if (nfds == 0) {
            break;
        }

        pr = as_poll(fds, (unsigned long)nfds, 100);
        if (!s->running) {
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

        as_mutex_lock(&s->lock);
        if (i_wake >= 0 && (fds[i_wake].revents & POLLIN)) {
            drain_wakeup(s);
        }
        if (i_listen >= 0 && (fds[i_listen].revents & POLLIN)) {
            handle_accept(s);
        }
        if (i_client >= 0 && s->client_fd != AS_INVALID_FD) {
            short rev = fds[i_client].revents;
            if (rev & (POLLIN | POLLHUP | POLLERR)) {
                handle_client_read(s);
            }
            if (s->client_fd != AS_INVALID_FD && (rev & POLLOUT)) {
                flush_send_unlocked(s);
            }
        }
        as_mutex_unlock(&s->lock);
    }
}

#ifdef _WIN32
static unsigned __stdcall poll_thread_main(void *arg) {
    poll_loop((as_socket *)arg);
    return 0;
}
#else
static void *poll_thread_main(void *arg) {
    poll_loop((as_socket *)arg);
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
        if (listen(fd, 1) != 0) {
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

static as_socket *as_socket_new(void) {
    as_socket *s = (as_socket *)calloc(1, sizeof(as_socket));
    if (!s) {
        return NULL;
    }
    s->listen_fd = AS_INVALID_FD;
    s->client_fd = AS_INVALID_FD;
    s->wakeup_rd = AS_INVALID_FD;
    s->wakeup_wr = AS_INVALID_FD;
    as_mutex_init(&s->lock);
    return s;
}

as_socket *as_socket_listen(const char *host, int port, char *err, size_t errlen) {
    as_socket *s;

    if (as_net_init() != 0) {
        AS_SETERR(err, errlen, "WSAStartup failed");
        return NULL;
    }
    if (!host || port <= 0 || port > 65535) {
        AS_SETERR(err, errlen, "invalid host/port");
        return NULL;
    }
    s = as_socket_new();
    if (!s) {
        AS_SETERR(err, errlen, "out of memory");
        return NULL;
    }
    if (create_wakeup_pair(&s->wakeup_rd, &s->wakeup_wr, err, errlen) != 0) {
        as_socket_destroy(s);
        return NULL;
    }
    s->listen_fd = create_listen_fd(host, port, err, errlen);
    if (s->listen_fd == AS_INVALID_FD) {
        as_socket_destroy(s);
        return NULL;
    }
    s->running = 1;
#ifdef _WIN32
    s->thread = (HANDLE)_beginthreadex(NULL, 0, poll_thread_main, s, 0, NULL);
    if (!s->thread) {
        AS_SETERR(err, errlen, "failed to start poll thread");
        s->running = 0;
        as_socket_destroy(s);
        return NULL;
    }
#else
    if (pthread_create(&s->thread, NULL, poll_thread_main, s) != 0) {
        AS_SETERR(err, errlen, "failed to start poll thread");
        s->running = 0;
        as_socket_destroy(s);
        return NULL;
    }
#endif
    s->thread_started = 1;
    return s;
}

void as_socket_wakeup(as_socket *s) {
    char b = 1;
    if (!s || s->wakeup_wr == AS_INVALID_FD) {
        return;
    }
    send(s->wakeup_wr, &b, 1, 0);
}

static void join_thread(as_socket *s) {
    if (!s->thread_started) {
        return;
    }
#ifdef _WIN32
    if (s->thread) {
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        s->thread = NULL;
    }
#else
    pthread_join(s->thread, NULL);
#endif
    s->thread_started = 0;
}

void as_socket_stop(as_socket *s) {
    if (!s || s->stopped) {
        return;
    }
    /* Stop flag + wakeup so WSAPoll/poll cannot sit until the 100ms timeout. */
    s->running = 0;
    as_socket_wakeup(s);
    join_thread(s);
    s->stopped = 1;

    as_mutex_lock(&s->lock);
    /*
     * Explicit close: if a client was still up, emit CLOSE once so Lua
     * on_close runs. sock_close() drains leftover events synchronously
     * after this returns (g_impl is then cleared).
     */
    if (s->client_fd != AS_INVALID_FD && !s->close_emitted) {
        emit_close_unlocked(s);
    } else {
        close_fd_slot(&s->client_fd);
    }
    s->send_len = 0;
    s->send_off = 0;
    close_fd_slot(&s->listen_fd);
    close_fd_slot(&s->wakeup_rd);
    close_fd_slot(&s->wakeup_wr);
    as_mutex_unlock(&s->lock);
}

void as_socket_destroy(as_socket *s) {
    if (!s) {
        return;
    }
    as_socket_stop(s);
    as_events_free(s->q, s->q_count);
    s->q = NULL;
    s->q_count = 0;
    s->q_cap = 0;
    free(s->send_buf);
    s->send_buf = NULL;
    as_mutex_destroy(&s->lock);
    free(s);
}

int as_socket_send(as_socket *s, const void *data, size_t len) {
    const char *p = (const char *)data;
    int need_pollout = 0;
    int fatal = 0;

    if (!s || s->stopped || !data) {
        return -1;
    }
    as_mutex_lock(&s->lock);
    if (s->client_fd == AS_INVALID_FD) {
        as_mutex_unlock(&s->lock);
        return -1;
    }
    /* Nonblocking write; remainder goes to send_buf and POLLOUT flush. */
    if (s->send_off >= s->send_len && len > 0) {
        int n = send(s->client_fd, p, (int)len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
        } else if (n < 0 && !AS_WOULDBLOCK()) {
            emit_close_unlocked(s);
            fatal = 1;
        }
    }
    if (!fatal && len > 0) {
        if (send_buf_append(s, p, len) != 0) {
            as_mutex_unlock(&s->lock);
            return -1;
        }
        need_pollout = 1;
    }
    as_mutex_unlock(&s->lock);
    if (need_pollout || fatal) {
        /* Wake poll so it rebuilds the fd set with POLLOUT, or notices CLOSE. */
        as_socket_wakeup(s);
    }
    return fatal ? -1 : 0;
}

as_event *as_socket_take_events(as_socket *s, size_t *out_n) {
    as_event *out;

    if (!s || !out_n) {
        if (out_n) {
            *out_n = 0;
        }
        return NULL;
    }
    as_mutex_lock(&s->lock);
    out = s->q;
    *out_n = s->q_count;
    s->q = NULL;
    s->q_count = 0;
    s->q_cap = 0;
    as_mutex_unlock(&s->lock);
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
