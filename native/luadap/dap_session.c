#include "dap_session.h"
#include "coro_registry.h"
#include "state_registry.h"
#include "dap_framing.h"
#include "dap_json.h"
#include "dap_sync.h"
#include "lua_debug.h"
#include "poll_loop.h"

#include <lua.h>
#include <lauxlib.h>
#include "lua_compat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
    int line;
    char *condition; /* nullable */
} dap_bp;

typedef struct {
    char *path;
    dap_bp *items;
    size_t n;
} dap_bp_file;

struct dap_session {
    int dap_conn_id; /* 0 = no debugger client */
    int listening;   /* 1 while as_listen is active */
    int client_open;
    int configured;
    int dead;
    int close_pending;
    int hook_installed;
    int next_ref;
    int seq;
    char host[128];
    int port;
    dap_recv_buf recv_buf;
    dap_bp_file *bp_files;
    size_t bp_n;
    size_t bp_cap;
};

static dap_session g_sess;

enum { DAP_PAUSED_MAX = 64 };

typedef struct {
    int thread_id;
    lua_State *L;
    lua_State *mainL; /* snapshotted on owner thread at pause */
    dap_cond cond;
    int cond_ready;
    int step_mode; /* DAP_STEP_* (copied to g_steps on step*) */
    int step_depth;
} dap_paused_entry;

enum { DAP_REQ_Q_MAX = 64 };

typedef struct {
    cJSON *msg;
} dap_queued_req;

typedef struct {
    int thread_id;
    lua_State *L;
    int step_mode;
    int step_depth;
} dap_step_slot;

static dap_paused_entry g_paused[DAP_PAUSED_MAX];
static int g_paused_n;
static dap_step_slot g_steps[DAP_PAUSED_MAX];
static dap_queued_req g_q[DAP_REQ_Q_MAX];
static int g_q_n;

static void dap_session_reset_client(lua_State *L, cJSON *disconnect_req);
static int send_event(const char *event, cJSON *body);
static void send_response(cJSON *req, cJSON *body, int success, const char *message);
static void dispatch(lua_State *L, cJSON *msg);
static int cmd_needs_lua(const char *cmd);
static void enqueue_req(cJSON *msg);
static void drain_lua_reqs_for(lua_State *self_L);
static void queue_clear(int reply);

static dap_paused_entry *paused_find(int tid) {
    int i;
    if (tid <= 0) return NULL;
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_paused[i].thread_id == tid)
            return &g_paused[i];
    }
    return NULL;
}

static dap_paused_entry *paused_find_L(lua_State *L) {
    int i;
    if (!L) return NULL;
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_paused[i].thread_id != 0 && g_paused[i].L == L)
            return &g_paused[i];
    }
    return NULL;
}

static dap_paused_entry *first_paused(void) {
    int i;
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_paused[i].thread_id != 0)
            return &g_paused[i];
    }
    return NULL;
}

static int paused_add(lua_State *L, int tid) {
    int i;
    dap_paused_entry *e;

    if (tid <= 0 || !L) return -1;
    e = paused_find(tid);
    if (e) {
        e->L = L;
        return 0;
    }
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_paused[i].thread_id != 0) continue;
        if (!g_paused[i].cond_ready) {
            dap_cond_init(&g_paused[i].cond);
            g_paused[i].cond_ready = 1;
        }
        g_paused[i].thread_id = tid;
        g_paused[i].L = L;
        g_paused[i].mainL = NULL;
        g_paused[i].step_mode = DAP_STEP_NONE;
        g_paused[i].step_depth = 0;
        g_paused_n++;
        return 0;
    }
    return -1;
}

static void paused_remove(int tid) {
    dap_paused_entry *e = paused_find(tid);
    if (!e) return;
    dap_cond_signal(&e->cond);
    e->thread_id = 0;
    e->L = NULL;
    e->mainL = NULL;
    e->step_mode = DAP_STEP_NONE;
    e->step_depth = 0;
    if (g_paused_n > 0) g_paused_n--;
}

static void paused_clear_all(void) {
    int i;
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_paused[i].thread_id != 0)
            dap_cond_signal(&g_paused[i].cond);
        g_paused[i].thread_id = 0;
        g_paused[i].L = NULL;
        g_paused[i].mainL = NULL;
        g_paused[i].step_mode = DAP_STEP_NONE;
        g_paused[i].step_depth = 0;
    }
    g_paused_n = 0;
}

static dap_step_slot *step_find_tid(int tid) {
    int i;
    if (tid <= 0) return NULL;
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_steps[i].thread_id == tid)
            return &g_steps[i];
    }
    return NULL;
}

static dap_step_slot *step_find_L(lua_State *L) {
    int i;
    if (!L) return NULL;
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_steps[i].thread_id != 0 && g_steps[i].L == L)
            return &g_steps[i];
    }
    return NULL;
}

static void step_set(int tid, lua_State *L, int mode, int depth) {
    dap_step_slot *s = step_find_tid(tid);
    int i;
    if (!s) {
        for (i = 0; i < DAP_PAUSED_MAX; i++) {
            if (g_steps[i].thread_id == 0) {
                s = &g_steps[i];
                break;
            }
        }
    }
    if (!s) return;
    s->thread_id = tid;
    s->L = L;
    s->step_mode = mode;
    s->step_depth = depth;
}

static void step_clear_tid(int tid) {
    dap_step_slot *s = step_find_tid(tid);
    if (!s) return;
    s->thread_id = 0;
    s->L = NULL;
    s->step_mode = DAP_STEP_NONE;
    s->step_depth = 0;
}

static void step_clear_all(void) {
    memset(g_steps, 0, sizeof(g_steps));
}

static int send_stopped_event(int tid, const char *reason) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return -1;
    if (!cJSON_AddStringToObject(body, "reason", reason ? reason : "breakpoint") ||
        !cJSON_AddNumberToObject(body, "threadId", (double)tid) ||
        !cJSON_AddBoolToObject(body, "allThreadsStopped", 0)) {
        cJSON_Delete(body);
        return -1;
    }
    return send_event("stopped", body);
}

dap_session *dap_session_get(void) { return &g_sess; }

static char *xstrdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

/* Path normalize (same as debugger.lua): strip leading @, \→/, lowercase
 * drive letter, strip trailing /. Returns malloc'd string; caller frees. */
char *dap_session_normalize_path(const char *path) {
    char *out;
    size_t n, i, w;

    if (!path) path = "";
    if (path[0] == '@') path++;
    n = strlen(path);
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    w = 0;
    for (i = 0; i < n; i++)
        out[w++] = (char)((path[i] == '\\') ? '/' : path[i]);
    out[w] = '\0';
    if (w >= 2 && out[1] == ':' && isalpha((unsigned char)out[0]))
        out[0] = (char)tolower((unsigned char)out[0]);
    while (w > 0 && out[w - 1] == '/') {
        out[--w] = '\0';
    }
    return out;
}

static char *trim_dup(const char *s) {
    size_t n;
    char *o;
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) n--;
    if (n == 0) return NULL;
    o = (char *)malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n);
    o[n] = '\0';
    return o;
}

static void bp_clear_file(dap_bp_file *f) {
    size_t i;
    if (!f) return;
    for (i = 0; i < f->n; i++)
        free(f->items[i].condition);
    free(f->items);
    f->items = NULL;
    f->n = 0;
}

static void bp_clear_all(void) {
    size_t i;
    for (i = 0; i < g_sess.bp_n; i++) {
        bp_clear_file(&g_sess.bp_files[i]);
        free(g_sess.bp_files[i].path);
    }
    free(g_sess.bp_files);
    g_sess.bp_files = NULL;
    g_sess.bp_n = 0;
    g_sess.bp_cap = 0;
}

static dap_bp_file *bp_find_or_add(const char *path) {
    size_t i;
    dap_bp_file *nf;
    dap_bp_file *f;
    size_t cap;

    for (i = 0; i < g_sess.bp_n; i++) {
        if (strcmp(g_sess.bp_files[i].path, path) == 0)
            return &g_sess.bp_files[i];
    }
    cap = g_sess.bp_cap;
    if (g_sess.bp_n >= cap) {
        cap = cap ? cap * 2 : 4;
        nf = (dap_bp_file *)realloc(g_sess.bp_files, cap * sizeof(*nf));
        if (!nf) return NULL;
        g_sess.bp_files = nf;
        g_sess.bp_cap = cap;
    }
    f = &g_sess.bp_files[g_sess.bp_n++];
    memset(f, 0, sizeof(*f));
    f->path = xstrdup(path);
    if (!f->path) {
        g_sess.bp_n--;
        return NULL;
    }
    return f;
}

/*
 * Send helpers:
 * - Do NOT free the request `req` passed to send_response.
 * - send_response / send_event free the response/event roots they build,
 *   including `body` (ownership of body transfers in).
 * - Printed JSON from cJSON_PrintUnformatted is freed after as_conn_send.
 */
static int send_raw(cJSON *obj) {
    char *body;
    char *frame;
    char hdr[64];
    int hdr_len;
    size_t body_len;
    size_t frame_len;
    int rc;

    if (!obj || g_sess.dap_conn_id <= 0) return -1;
    g_sess.seq += 1;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, "seq");
    if (!cJSON_AddNumberToObject(obj, "seq", (double)g_sess.seq))
        return -1;
    body = dap_json_print_unformatted(obj);
    if (!body) return -1;
    body_len = strlen(body);
    hdr_len = snprintf(hdr, sizeof(hdr), "Content-Length: %zu\r\n\r\n", body_len);
    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(hdr)) {
        cJSON_free(body);
        return -1;
    }
    frame_len = (size_t)hdr_len + body_len;
    frame = (char *)malloc(frame_len);
    if (!frame) {
        cJSON_free(body);
        return -1;
    }
    memcpy(frame, hdr, (size_t)hdr_len);
    memcpy(frame + (size_t)hdr_len, body, body_len);
    cJSON_free(body);
    rc = as_conn_send(g_sess.dap_conn_id, frame, frame_len);
    free(frame);
    return rc;
}

static void send_response(cJSON *req, cJSON *body, int success, const char *message) {
    cJSON *resp;
    cJSON *seq;
    cJSON *cmd;

    resp = cJSON_CreateObject();
    if (!resp) {
        cJSON_Delete(body);
        return;
    }
    cJSON_AddStringToObject(resp, "type", "response");
    seq = req ? cJSON_GetObjectItemCaseSensitive(req, "seq") : NULL;
    cJSON_AddNumberToObject(resp, "request_seq",
                            (seq && cJSON_IsNumber(seq)) ? seq->valuedouble : 0);
    cJSON_AddBoolToObject(resp, "success", success ? 1 : 0);
    cmd = req ? cJSON_GetObjectItemCaseSensitive(req, "command") : NULL;
    cJSON_AddStringToObject(resp, "command",
                            (cmd && cJSON_IsString(cmd) && cmd->valuestring)
                                ? cmd->valuestring
                                : "");
    if (message)
        cJSON_AddStringToObject(resp, "message", message);
    if (!body)
        body = cJSON_CreateObject();
    if (body)
        cJSON_AddItemToObject(resp, "body", body);
    send_raw(resp);
    cJSON_Delete(resp);
}

static int send_event(const char *event, cJSON *body) {
    cJSON *obj = cJSON_CreateObject();
    int rc;
    if (!obj) {
        cJSON_Delete(body);
        return -1;
    }
    cJSON_AddStringToObject(obj, "type", "event");
    cJSON_AddStringToObject(obj, "event", event ? event : "");
    if (!body)
        body = cJSON_CreateObject();
    if (body)
        cJSON_AddItemToObject(obj, "body", body);
    rc = send_raw(obj);
    cJSON_Delete(obj);
    return rc;
}

static const char *msg_command(cJSON *msg) {
    cJSON *cmdj = msg ? cJSON_GetObjectItemCaseSensitive(msg, "command") : NULL;
    return (cmdj && cJSON_IsString(cmdj) && cmdj->valuestring) ? cmdj->valuestring
                                                              : NULL;
}

static int msg_thread_id(cJSON *msg) {
    cJSON *args = msg ? cJSON_GetObjectItemCaseSensitive(msg, "arguments") : NULL;
    cJSON *tj = args ? cJSON_GetObjectItemCaseSensitive(args, "threadId") : NULL;
    if (tj && cJSON_IsNumber(tj))
        return (int)tj->valuedouble;
    return 0;
}

/* Owner-thread only. Do not call on another state's L. */
static lua_State *owner_main_of(lua_State *L) {
#if LUA_VERSION_NUM >= 502
    lua_State *m;
    if (!L) return NULL;
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    m = lua_tothread(L, -1);
    lua_pop(L, 1);
    return m ? m : L;
#else
    return L;
#endif
}

static int L_related(lua_State *self_L, lua_State *target, lua_State *target_main) {
    if (!self_L || !target) return 0;
    if (self_L == target) return 1;
    /* Updater is the snapshotted main of the paused state (same VM). */
    if (target_main && self_L == target_main) return 1;
    return 0;
}

static dap_paused_entry *paused_related_to(lua_State *self_L) {
    dap_paused_entry *e;
    int i;

    e = paused_find_L(self_L);
    if (e) return e;
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_paused[i].thread_id != 0 &&
            L_related(self_L, g_paused[i].L, g_paused[i].mainL))
            return &g_paused[i];
    }
    return NULL;
}

static int cmd_needs_lua(const char *cmd) {
    if (!cmd) return 0;
    return strcmp(cmd, "stackTrace") == 0 || strcmp(cmd, "scopes") == 0 ||
           strcmp(cmd, "variables") == 0 || strcmp(cmd, "evaluate") == 0;
}

static int lua_req_ready_for(lua_State *self_L, cJSON *msg) {
    int tid = msg_thread_id(msg);
    dap_paused_entry *e;

    if (tid > 0) {
        e = paused_find(tid);
        if (!e) return 1; /* empty / error path; no Lua walk */
        return L_related(self_L, e->L, e->mainL);
    }
    if (g_paused_n == 0) return 1;
    return paused_related_to(self_L) != NULL;
}

static void queue_clear(int reply) {
    int i;
    for (i = 0; i < g_q_n; i++) {
        if (!g_q[i].msg) continue;
        if (reply)
            send_response(g_q[i].msg, NULL, 0, "disconnected");
        cJSON_Delete(g_q[i].msg);
        g_q[i].msg = NULL;
    }
    g_q_n = 0;
}

static void enqueue_req(cJSON *msg) {
    int tid;
    int i;

    if (!msg) return;
    if (g_q_n >= DAP_REQ_Q_MAX) {
        send_response(msg, NULL, 0, "request queue full");
        cJSON_Delete(msg);
        return;
    }
    g_q[g_q_n].msg = msg;
    g_q_n++;

    tid = msg_thread_id(msg);
    if (tid > 0) {
        dap_paused_entry *e = paused_find(tid);
        if (e) dap_cond_signal(&e->cond);
        return;
    }
    for (i = 0; i < DAP_PAUSED_MAX; i++) {
        if (g_paused[i].thread_id != 0)
            dap_cond_signal(&g_paused[i].cond);
    }
}

static void drain_lua_reqs_for(lua_State *self_L) {
    int i = 0;

    while (i < g_q_n) {
        cJSON *msg = g_q[i].msg;
        if (!lua_req_ready_for(self_L, msg)) {
            i++;
            continue;
        }
        if (i + 1 < g_q_n)
            memmove(&g_q[i], &g_q[i + 1],
                    (size_t)(g_q_n - i - 1) * sizeof(g_q[0]));
        g_q_n--;
        g_q[g_q_n].msg = NULL;
        dispatch(self_L, msg);
        cJSON_Delete(msg);
        if (g_sess.dead || !g_sess.client_open) break;
    }
}

static void handle_initialize(cJSON *req) {
    cJSON *caps = cJSON_CreateObject();
    if (caps) {
        cJSON_AddBoolToObject(caps, "supportsConfigurationDoneRequest", 1);
        cJSON_AddBoolToObject(caps, "supportsSetVariable", 0);
        cJSON_AddBoolToObject(caps, "supportsConditionalBreakpoints", 1);
        cJSON_AddBoolToObject(caps, "supportsEvaluateForHovers", 1);
    }
    send_response(req, caps, 1, NULL);
    send_event("initialized", NULL);
}

static void handle_attach(cJSON *req) {
    send_response(req, cJSON_CreateObject(), 1, NULL);
}

static void handle_threads(lua_State *L, cJSON *req) {
    cJSON *body = cJSON_CreateObject();
    cJSON *arr;
    (void)L;
    if (!body) {
        send_response(req, NULL, 0, "oom");
        return;
    }
    arr = cJSON_AddArrayToObject(body, "threads");
    if (arr)
        coro_registry_append_threads_json(arr);
    send_response(req, body, 1, NULL);
}

static void handle_set_exception_breakpoints(cJSON *req) {
    send_response(req, cJSON_CreateObject(), 1, NULL);
}

static void handle_set_breakpoints(cJSON *req) {
    cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    cJSON *src = args ? cJSON_GetObjectItemCaseSensitive(args, "source") : NULL;
    cJSON *pathj = src ? cJSON_GetObjectItemCaseSensitive(src, "path") : NULL;
    cJSON *in = args ? cJSON_GetObjectItemCaseSensitive(args, "breakpoints") : NULL;
    const char *raw = (pathj && cJSON_IsString(pathj) && pathj->valuestring)
                          ? pathj->valuestring
                          : "";
    char *path = dap_session_normalize_path(raw);
    dap_bp_file *file;
    cJSON *out;
    cJSON *body;
    int n;
    int i;

    if (!path) {
        send_response(req, NULL, 0, "oom");
        return;
    }
    file = bp_find_or_add(path);
    free(path);
    if (!file) {
        send_response(req, NULL, 0, "oom");
        return;
    }
    bp_clear_file(file);

    n = cJSON_IsArray(in) ? cJSON_GetArraySize(in) : 0;
    if (n < 0) n = 0;
    if (n > 0) {
        file->items = (dap_bp *)calloc((size_t)n, sizeof(dap_bp));
        if (!file->items) {
            send_response(req, NULL, 0, "oom");
            return;
        }
    }

    body = cJSON_CreateObject();
    out = body ? cJSON_AddArrayToObject(body, "breakpoints") : NULL;
    for (i = 0; i < n; i++) {
        cJSON *bp = cJSON_GetArrayItem(in, i);
        cJSON *linej = bp ? cJSON_GetObjectItemCaseSensitive(bp, "line") : NULL;
        cJSON *condj = bp ? cJSON_GetObjectItemCaseSensitive(bp, "condition") : NULL;
        cJSON *verified;
        int line;
        char *cond = NULL;

        if (!linej || !cJSON_IsNumber(linej)) continue;
        line = (int)linej->valuedouble;
        if (condj && cJSON_IsString(condj) && condj->valuestring)
            cond = trim_dup(condj->valuestring);

        if (file->items) {
            file->items[file->n].line = line;
            file->items[file->n].condition = cond;
            file->n++;
        } else {
            free(cond);
            cond = NULL;
        }

        verified = cJSON_CreateObject();
        if (verified) {
            cJSON_AddNumberToObject(verified, "line", line);
            cJSON_AddBoolToObject(verified, "verified", 1);
            if (cond)
                cJSON_AddStringToObject(verified, "condition", cond);
            if (out) cJSON_AddItemToArray(out, verified);
            else cJSON_Delete(verified);
        }
    }
    send_response(req, body, 1, NULL);
}

static void handle_configuration_done(cJSON *req) {
    send_response(req, cJSON_CreateObject(), 1, NULL);
    g_sess.configured = 1;
    /* Do not install the hook here: host thread update/wait installs it. */
}

static int resolve_target_tid(cJSON *req, int *out_tid) {
    cJSON *args = req ? cJSON_GetObjectItemCaseSensitive(req, "arguments") : NULL;
    cJSON *tj = args ? cJSON_GetObjectItemCaseSensitive(args, "threadId") : NULL;
    dap_paused_entry *e;

    if (tj && cJSON_IsNumber(tj)) {
        *out_tid = (int)tj->valuedouble;
        return 0;
    }
    if (g_paused_n == 1) {
        e = first_paused();
        if (!e) return -1;
        *out_tid = e->thread_id;
        return 0;
    }
    return -1;
}

static void handle_continue(cJSON *req) {
    int tid;
    cJSON *body;

    if (resolve_target_tid(req, &tid) != 0 || !paused_find(tid)) {
        send_response(req, NULL, 0, "thread not paused");
        return;
    }
    step_clear_tid(tid);
    paused_remove(tid);
    body = cJSON_CreateObject();
    if (body)
        cJSON_AddBoolToObject(body, "allThreadsContinued", 0);
    send_response(req, body, 1, NULL);
}

static void handle_step_cmd(lua_State *L, cJSON *req, int mode) {
    int tid;
    dap_paused_entry *e;
    lua_State *target;

    (void)L;
    if (resolve_target_tid(req, &tid) != 0) {
        send_response(req, NULL, 0, "thread not paused");
        return;
    }
    e = paused_find(tid);
    if (!e) {
        send_response(req, NULL, 0, "thread not paused");
        return;
    }
    target = e->L;
    /* Depth snapshotted on the owner thread at pause; do not walk Lua here. */
    step_set(tid, target, mode, e->step_depth);
    paused_remove(tid);
    send_response(req, cJSON_CreateObject(), 1, NULL);
}

static void handle_next(lua_State *L, cJSON *req) {
    handle_step_cmd(L, req, DAP_STEP_OVER);
}

static void handle_step_in(lua_State *L, cJSON *req) {
    handle_step_cmd(L, req, DAP_STEP_IN);
}

static void handle_step_out(lua_State *L, cJSON *req) {
    handle_step_cmd(L, req, DAP_STEP_OUT);
}

static void handle_stack_trace(lua_State *L, cJSON *req) {
    cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    cJSON *tidj = args ? cJSON_GetObjectItemCaseSensitive(args, "threadId") : NULL;
    int tid = (tidj && cJSON_IsNumber(tidj)) ? (int)tidj->valuedouble : 1;
    lua_State *target = coro_registry_state_for(tid);
    dap_paused_entry *e;
    lua_State *walk_L;
    cJSON *body;

    if (!target) {
        send_response(req, NULL, 0, "unknown thread");
        return;
    }
    e = paused_find(tid);
    if (!e || !L_related(L, e->L, e->mainL)) {
        body = cJSON_CreateObject();
        if (body) {
            cJSON_AddArrayToObject(body, "stackFrames");
            cJSON_AddNumberToObject(body, "totalFrames", 0);
        }
        send_response(req, body, 1, NULL);
        return;
    }
    walk_L = e->L;
    dap_mutex_unlock();
    body = lua_debug_stack_frames(walk_L);
    dap_mutex_lock();
    send_response(req, body, 1, NULL);
}

static void handle_scopes(cJSON *req) {
    cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    cJSON *fid = args ? cJSON_GetObjectItemCaseSensitive(args, "frameId") : NULL;
    int frame_id = (fid && cJSON_IsNumber(fid)) ? (int)fid->valuedouble : 0;
    cJSON *body = cJSON_CreateObject();
    cJSON *scopes = body ? cJSON_AddArrayToObject(body, "scopes") : NULL;
    cJSON *loc = cJSON_CreateObject();
    cJSON *up = cJSON_CreateObject();

    if (loc) {
        cJSON_AddStringToObject(loc, "name", "Locals");
        cJSON_AddNumberToObject(loc, "variablesReference", 100000 + frame_id);
        cJSON_AddBoolToObject(loc, "expensive", 0);
        if (scopes)
            cJSON_AddItemToArray(scopes, loc);
        else
            cJSON_Delete(loc);
    }
    if (up) {
        cJSON_AddStringToObject(up, "name", "Upvalues");
        cJSON_AddNumberToObject(up, "variablesReference", 200000 + frame_id);
        cJSON_AddBoolToObject(up, "expensive", 0);
        if (scopes)
            cJSON_AddItemToArray(scopes, up);
        else
            cJSON_Delete(up);
    }
    send_response(req, body, 1, NULL);
}

static void handle_variables(lua_State *L, cJSON *req) {
    cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    cJSON *refj = args ? cJSON_GetObjectItemCaseSensitive(args, "variablesReference")
                       : NULL;
    int ref = (refj && cJSON_IsNumber(refj)) ? (int)refj->valuedouble : 0;
    int tid = msg_thread_id(req);
    dap_paused_entry *pe = tid > 0 ? paused_find(tid) : paused_related_to(L);
    lua_State *target = (pe && L_related(L, pe->L, pe->mainL)) ? pe->L : NULL;
    cJSON *body;

    if (!target) {
        body = cJSON_CreateObject();
        if (body) cJSON_AddArrayToObject(body, "variables");
        send_response(req, body, 1, NULL);
        return;
    }
    dap_mutex_unlock();
    body = lua_debug_collect_variables(target, ref);
    dap_mutex_lock();
    send_response(req, body, 1, NULL);
}

static void handle_evaluate(lua_State *L, cJSON *req) {
    cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    cJSON *exprj = args ? cJSON_GetObjectItemCaseSensitive(args, "expression") : NULL;
    cJSON *fidj = args ? cJSON_GetObjectItemCaseSensitive(args, "frameId") : NULL;
    cJSON *ctxj = args ? cJSON_GetObjectItemCaseSensitive(args, "context") : NULL;
    const char *expr = (exprj && cJSON_IsString(exprj) && exprj->valuestring)
                           ? exprj->valuestring
                           : NULL;
    int frame_id = (fidj && cJSON_IsNumber(fidj)) ? (int)fidj->valuedouble : 0;
    const char *ctx = (ctxj && cJSON_IsString(ctxj) && ctxj->valuestring)
                          ? ctxj->valuestring
                          : "watch";
    char *err = NULL;
    cJSON *body;

    {
        int tid = msg_thread_id(req);
        dap_paused_entry *pe = tid > 0 ? paused_find(tid) : paused_related_to(L);
        lua_State *target = (pe && L_related(L, pe->L, pe->mainL)) ? pe->L : NULL;
        if (!target) {
            send_response(req, NULL, 0, "not paused");
            return;
        }
        dap_mutex_unlock();
        body = lua_debug_evaluate(target, expr, frame_id, ctx, &err);
        dap_mutex_lock();
    }
    if (!body) {
        send_response(req, NULL, 0, err ? err : "evaluate failed");
        free(err);
        return;
    }
    send_response(req, body, 1, NULL);
}

static void handle_disconnect(lua_State *L, cJSON *req) {
    /* Keep listen alive so the host can accept another VS Code F5 attach. */
    dap_session_reset_client(L, req);
}

static void dispatch(lua_State *L, cJSON *msg) {
    cJSON *type;
    cJSON *cmdj;
    const char *cmd;

    if (!msg) return;
    type = cJSON_GetObjectItemCaseSensitive(msg, "type");
    if (!cJSON_IsString(type) || !type->valuestring ||
        strcmp(type->valuestring, "request") != 0)
        return;
    cmdj = cJSON_GetObjectItemCaseSensitive(msg, "command");
    cmd = (cmdj && cJSON_IsString(cmdj) && cmdj->valuestring) ? cmdj->valuestring
                                                             : NULL;
    if (!cmd) {
        send_response(msg, cJSON_CreateObject(), 0, "not supported: nil");
        return;
    }
    if (strcmp(cmd, "initialize") == 0)
        handle_initialize(msg);
    else if (strcmp(cmd, "attach") == 0 || strcmp(cmd, "launch") == 0)
        /* Extension may spawn lua-runner then send DAP launch; treat like attach. */
        handle_attach(msg);
    else if (strcmp(cmd, "threads") == 0)
        handle_threads(L, msg);
    else if (strcmp(cmd, "setExceptionBreakpoints") == 0)
        handle_set_exception_breakpoints(msg);
    else if (strcmp(cmd, "setBreakpoints") == 0)
        handle_set_breakpoints(msg);
    else if (strcmp(cmd, "configurationDone") == 0)
        handle_configuration_done(msg);
    else if (strcmp(cmd, "continue") == 0)
        handle_continue(msg);
    else if (strcmp(cmd, "next") == 0)
        handle_next(L, msg);
    else if (strcmp(cmd, "stepIn") == 0)
        handle_step_in(L, msg);
    else if (strcmp(cmd, "stepOut") == 0)
        handle_step_out(L, msg);
    else if (strcmp(cmd, "stackTrace") == 0)
        handle_stack_trace(L, msg);
    else if (strcmp(cmd, "scopes") == 0)
        handle_scopes(msg);
    else if (strcmp(cmd, "variables") == 0)
        handle_variables(L, msg);
    else if (strcmp(cmd, "evaluate") == 0)
        handle_evaluate(L, msg);
    else if (strcmp(cmd, "disconnect") == 0 || strcmp(cmd, "terminate") == 0)
        handle_disconnect(L, msg);
    else {
        char buf[160];
        snprintf(buf, sizeof(buf), "not supported: %s", cmd);
        send_response(msg, cJSON_CreateObject(), 0, buf);
    }
}

int dap_session_is_dead(void) { return g_sess.dead; }

int dap_session_client_open(void) { return g_sess.client_open; }

int dap_session_hooks_active(void) { return g_sess.hook_installed && !g_sess.dead; }

int dap_session_is_paused(void) { return g_paused_n > 0; }

int dap_session_paused_contains(int thread_id) {
    int found;
    dap_mutex_init();
    dap_mutex_lock();
    found = paused_find(thread_id) != NULL;
    dap_mutex_unlock();
    return found;
}

int dap_session_is_L_paused(lua_State *L) {
    int found;
    dap_mutex_init();
    dap_mutex_lock();
    found = paused_find_L(L) != NULL;
    dap_mutex_unlock();
    return found;
}

int dap_session_pause_enter(lua_State *L, int thread_id, const char *reason) {
    int rc;
    int depth = lua_debug_current_depth(L);
    lua_State *mainL = owner_main_of(L);
    dap_paused_entry *e;

    dap_session_reset_var_maps(L);
    dap_mutex_init();
    dap_mutex_lock();
    if (paused_add(L, thread_id) != 0) {
        dap_mutex_unlock();
        return -1;
    }
    e = paused_find(thread_id);
    if (e) {
        e->mainL = mainL;
        e->step_depth = depth;
    }
    rc = send_stopped_event(thread_id, reason);
    dap_mutex_unlock();
    return rc;
}

void dap_session_pause_wait_idle(int thread_id) {
    dap_paused_entry *e;

    dap_mutex_init();
    dap_mutex_lock();
    e = paused_find(thread_id);
    if (e)
        dap_cond_timedwait(&e->cond, 1);
    dap_mutex_unlock();
}

void dap_session_pause_wait(int thread_id) { dap_session_pause_wait_idle(thread_id); }

int dap_session_resume_thread(int thread_id) {
    int rc = -1;

    dap_mutex_init();
    dap_mutex_lock();
    if (paused_find(thread_id)) {
        paused_remove(thread_id);
        rc = 0;
    }
    dap_mutex_unlock();
    return rc;
}

lua_State *dap_session_paused_L_for(int thread_id) {
    dap_paused_entry *e;
    lua_State *L = NULL;

    dap_mutex_init();
    dap_mutex_lock();
    e = paused_find(thread_id);
    if (e) L = e->L;
    dap_mutex_unlock();
    return L;
}

int dap_session_step_mode_of(lua_State *L) {
    dap_step_slot *s;
    int mode = DAP_STEP_NONE;

    dap_mutex_init();
    dap_mutex_lock();
    s = step_find_L(L);
    if (s) mode = s->step_mode;
    dap_mutex_unlock();
    return mode;
}

int dap_session_step_depth_of(lua_State *L) {
    dap_step_slot *s;
    int depth = 0;

    dap_mutex_init();
    dap_mutex_lock();
    s = step_find_L(L);
    if (s) depth = s->step_depth;
    dap_mutex_unlock();
    return depth;
}

void dap_session_clear_step_of(lua_State *L) {
    dap_step_slot *s;

    dap_mutex_init();
    dap_mutex_lock();
    s = step_find_L(L);
    if (s) {
        s->thread_id = 0;
        s->L = NULL;
        s->step_mode = DAP_STEP_NONE;
        s->step_depth = 0;
    }
    dap_mutex_unlock();
}

void dap_session_reset_var_maps(lua_State *L) {
    g_sess.next_ref = 1000;
    lua_debug_reset_var_maps(L);
}

static const dap_bp *bp_at(const char *norm_path, int line) {
    size_t i, j;
    if (!norm_path) return NULL;
    for (i = 0; i < g_sess.bp_n; i++) {
        if (!g_sess.bp_files[i].path ||
            strcmp(g_sess.bp_files[i].path, norm_path) != 0)
            continue;
        for (j = 0; j < g_sess.bp_files[i].n; j++) {
            if (g_sess.bp_files[i].items[j].line == line)
                return &g_sess.bp_files[i].items[j];
        }
    }
    return NULL;
}

int dap_session_bp_should_stop(const char *norm_path, int line) {
    return bp_at(norm_path, line) != NULL;
}

const char *dap_session_bp_condition(const char *norm_path, int line) {
    const dap_bp *bp = bp_at(norm_path, line);
    return bp ? bp->condition : NULL;
}

/* End the current DAP *client* session but keep the TCP listen socket so a
 * long-running host (start(..., false) + update loop) can accept F5 again.
 * - If handshake never completed: set configured=1 so start(wait=true) returns.
 * - If already configured: set configured=0 and drop hooks for a clean re-attach.
 */
static void dap_session_reset_client(lua_State *L, cJSON *disconnect_req) {
    int can_send;
    int was_configured;

    if (g_sess.dead || !g_sess.listening)
        return;

    was_configured = g_sess.configured;
    can_send = g_sess.client_open && g_sess.dap_conn_id > 0;
    queue_clear(can_send);
    if (can_send) {
        if (disconnect_req)
            send_response(disconnect_req, cJSON_CreateObject(), 1, NULL);
        send_event("terminated", cJSON_CreateObject());
    }

    if (g_sess.dap_conn_id > 0) {
        as_conn_close(g_sess.dap_conn_id);
        g_sess.dap_conn_id = 0;
    }

    g_sess.close_pending = 0;
    paused_clear_all();
    step_clear_all();
    g_sess.client_open = 0;
    g_sess.seq = 0;

    {
        int si, sn = state_registry_count();
        for (si = 0; si < sn; si++)
            lua_debug_clear_hook(state_registry_main_at(si));
    }
    coro_registry_clear_hooks_all();
    lua_debug_reset_var_maps(L);
    g_sess.hook_installed = 0;
    dap_recv_buf_free(&g_sess.recv_buf);
    dap_recv_buf_init(&g_sess.recv_buf);
    bp_clear_all();

    /* Do not touch coro wrappers / registry — process lifetime. */
    if (was_configured)
        g_sess.configured = 0;
    else
        g_sess.configured = 1; /* unblock start(wait=true) mid-handshake */
}

void dap_session_shutdown(lua_State *L, cJSON *disconnect_req) {
    int conn_id;
    int can_send;
    as_event *leftover;
    size_t n = 0;

    if (g_sess.dead) {
        queue_clear(0);
        paused_clear_all();
        step_clear_all();
        g_sess.configured = 1;
        return;
    }
    g_sess.dead = 1;
    g_sess.close_pending = 0;
    paused_clear_all();
    step_clear_all();
    g_sess.configured = 1;

    /* Gold: reply + terminated while the client is still marked open, then
     * drop the hook and engine. CLOSE+MESSAGE batches drain before this. */
    conn_id = g_sess.dap_conn_id;
    can_send = (conn_id > 0 && g_sess.client_open);
    queue_clear(can_send);
    if (can_send) {
        if (disconnect_req)
            send_response(disconnect_req, cJSON_CreateObject(), 1, NULL);
        send_event("terminated", cJSON_CreateObject());
    }

    {
        int si, sn = state_registry_count();
        for (si = 0; si < sn; si++) {
            lua_State *m = state_registry_main_at(si);
            lua_debug_clear_hook(m);
            coro_registry_uninstall_wrappers(m);
        }
    }
    lua_debug_reset_var_maps(L);
    g_sess.hook_installed = 0;
    coro_registry_clear(L);
    state_registry_clear();
    g_sess.client_open = 0;
    g_sess.dap_conn_id = 0;
    g_sess.listening = 0;
    if (conn_id > 0)
        as_conn_close(conn_id);
    as_server_close();
    as_engine_stop();
    leftover = as_take_events(&n);
    as_events_free(leftover, n);
    dap_recv_buf_free(&g_sess.recv_buf);
    bp_clear_all();
}

int dap_session_update(lua_State *L) {
    size_t n = 0;
    as_event *evs;
    size_t i;
    int k;
    int rc = 0;

    dap_mutex_init();
    dap_mutex_lock();

    if (!g_sess.listening || g_sess.dead) goto out;

    evs = as_take_events(&n);
    for (i = 0; i < n; i++) {
        if (evs[i].type == AS_EVT_ACCEPT) {
            if (g_sess.dap_conn_id == 0) {
                g_sess.dap_conn_id = evs[i].conn_id;
                g_sess.client_open = 1;
            } else {
                as_conn_close(evs[i].conn_id); /* single debugger client */
            }
        } else if (evs[i].type == AS_EVT_MESSAGE) {
            if (g_sess.dap_conn_id > 0 && evs[i].conn_id == g_sess.dap_conn_id) {
                if (dap_recv_buf_append(&g_sess.recv_buf, evs[i].payload,
                                        evs[i].len) != 0) {
                    as_events_free(evs, n);
                    dap_session_reset_client(L, NULL);
                    goto out;
                }
            }
        } else if (evs[i].type == AS_EVT_CLOSE) {
            if (g_sess.dap_conn_id > 0 && evs[i].conn_id == g_sess.dap_conn_id)
                g_sess.close_pending = 1;
        }
    }
    as_events_free(evs, n);

    for (k = 0; k < 32; k++) {
        char *json = NULL;
        size_t jlen = 0;
        int pr;
        cJSON *msg;

        pr = dap_try_parse_frame(&g_sess.recv_buf, &json, &jlen);
        if (pr == 0) break;
        if (pr < 0) {
            free(json);
            dap_session_reset_client(L, NULL);
            goto out;
        }
        msg = dap_json_parse(json, jlen);
        free(json);
        if (!msg) {
            dap_session_reset_client(L, NULL);
            goto out;
        }
        if (cmd_needs_lua(msg_command(msg)))
            enqueue_req(msg);
        else {
            dispatch(L, msg);
            cJSON_Delete(msg);
        }
        if (g_sess.dead || !g_sess.client_open) break;
    }

    drain_lua_reqs_for(L);

    if (g_sess.close_pending) {
        g_sess.close_pending = 0;
        if (!g_sess.dead)
            dap_session_reset_client(L, NULL);
    } else if (g_sess.configured && !g_sess.hook_installed && g_sess.client_open &&
               !g_sess.dead) {
        lua_debug_install_hook(L);
        g_sess.hook_installed = 1;
        coro_registry_install_hooks_all();
    }
    if (!g_sess.dead)
        coro_registry_purge_dead(L);

out:
    dap_mutex_unlock();
    return rc;
}

static void start_wait_configured(lua_State *L) {
    while (!g_sess.configured && !g_sess.dead) {
        if (dap_session_update(L) != 0) {
            dap_session_shutdown(L, NULL);
            break;
        }
        short_sleep();
    }
    /* Host thread after wait: same thread that will run debugee code. */
    if (g_sess.client_open && !g_sess.dead && !g_sess.hook_installed) {
        lua_debug_install_hook(L);
        g_sess.hook_installed = 1;
        coro_registry_install_hooks_all();
    } else if (g_sess.client_open && !g_sess.dead && g_sess.hook_installed) {
        lua_debug_install_hook(L);
    }
}

static int start_join_state(lua_State *L, const char *name) {
    if (state_registry_has(L)) return 0;
    if (state_registry_add(L, name) == 0) return -1;
    if (coro_registry_track(L, L, name) == 0) return -1;
    if (g_sess.hook_installed)
        lua_debug_install_hook(L);
    coro_registry_install_wrappers(L);
    return 0;
}

int dap_session_start(lua_State *L, const char *host, int port, int wait) {
    return dap_session_start_ex(L, host, port, wait, NULL);
}

int dap_session_start_ex(lua_State *L, const char *host, int port, int wait,
                         const char *name) {
    char err[256];
    int rc = 0;

    if (!host) host = "";
    if (name && !name[0]) name = NULL;

    dap_mutex_init();
    dap_mutex_lock();

    if (g_sess.listening) {
        if (strcmp(host, g_sess.host) != 0 || port != g_sess.port) {
            rc = -1;
            goto out;
        }
        if (start_join_state(L, name) != 0) {
            rc = -1;
            goto out;
        }
        if (wait)
            start_wait_configured(L);
        goto out;
    }

    if (g_sess.recv_buf.data || g_sess.bp_files)
        dap_session_shutdown(L, NULL);
    bp_clear_all();
    dap_recv_buf_free(&g_sess.recv_buf);
    memset(&g_sess, 0, sizeof(g_sess));
    paused_clear_all();
    step_clear_all();
    queue_clear(0);
    state_registry_clear();
    dap_recv_buf_init(&g_sess.recv_buf);
    g_sess.next_ref = 1000;
    g_sess.dap_conn_id = 0;

    if (as_net_init() != 0) {
        rc = -1;
        goto out;
    }
    if (as_listen(host, port, err, sizeof(err)) != 0) {
        fprintf(stderr, "[luadap] listen failed: %s\n", err);
        rc = -1;
        goto out;
    }
    snprintf(g_sess.host, sizeof(g_sess.host), "%s", host);
    g_sess.port = port;
    g_sess.listening = 1;
    fprintf(stderr, "[luadap] listening on %s:%d\n", host, port);

    if (state_registry_add(L, name) == 0) {
        rc = -1;
        goto out;
    }
    coro_registry_clear(L);
    coro_registry_track(L, L, name ? name : "main");
    coro_registry_install_wrappers(L);

    if (wait)
        start_wait_configured(L);

out:
    dap_mutex_unlock();
    return rc;
}
