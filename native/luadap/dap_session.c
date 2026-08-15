#include "dap_session.h"
#include "coro_registry.h"
#include "dap_framing.h"
#include "dap_json.h"
#include "lua_debug.h"
#include "poll_loop.h"

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
    int paused;
    int paused_thread_id;
    lua_State *paused_L;
    int step; /* DAP_STEP_* */
    int step_depth;
    lua_State *step_L;
    int next_ref;
    int seq;
    dap_recv_buf recv_buf;
    dap_bp_file *bp_files;
    size_t bp_n;
    size_t bp_cap;
};

static dap_session g_sess;

static void dap_session_reset_client(lua_State *L, cJSON *disconnect_req);

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

static void handle_initialize(cJSON *req) {
    cJSON *caps = cJSON_CreateObject();
    if (caps) {
        cJSON_AddBoolToObject(caps, "supportsConfigurationDoneRequest", 1);
        cJSON_AddBoolToObject(caps, "supportsSetVariable", 0);
        cJSON_AddBoolToObject(caps, "supportsConditionalBreakpoints", 1);
        cJSON_AddBoolToObject(caps, "supportsEvaluateForHovers", 0);
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
    if (!body) {
        send_response(req, NULL, 0, "oom");
        return;
    }
    arr = cJSON_AddArrayToObject(body, "threads");
    if (arr) {
        coro_registry_purge_dead(L);
        coro_registry_append_threads_json(arr);
    }
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

static lua_State *step_target_L(lua_State *L) {
    lua_State *pl = dap_session_paused_L();
    return pl ? pl : L;
}

static void clear_paused_thread(void) {
    g_sess.paused = 0;
    g_sess.paused_L = NULL;
    g_sess.paused_thread_id = 0;
}

static void handle_continue(cJSON *req) {
    cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    cJSON *tidj = args ? cJSON_GetObjectItemCaseSensitive(args, "threadId") : NULL;
    cJSON *body;

    if (tidj && cJSON_IsNumber(tidj) &&
        (int)tidj->valuedouble != dap_session_paused_thread_id()) {
        send_response(req, NULL, 0, "thread not paused");
        return;
    }
    body = cJSON_CreateObject();
    g_sess.step = DAP_STEP_NONE;
    g_sess.step_depth = 0;
    g_sess.step_L = NULL;
    clear_paused_thread();
    if (body)
        cJSON_AddBoolToObject(body, "allThreadsContinued", 1);
    send_response(req, body, 1, NULL);
}

static void handle_next(lua_State *L, cJSON *req) {
    lua_State *target = step_target_L(L);
    g_sess.step = DAP_STEP_OVER;
    g_sess.step_depth = lua_debug_current_depth(target);
    g_sess.step_L = target;
    clear_paused_thread();
    send_response(req, cJSON_CreateObject(), 1, NULL);
}

static void handle_step_in(lua_State *L, cJSON *req) {
    lua_State *target = step_target_L(L);
    g_sess.step = DAP_STEP_IN;
    g_sess.step_depth = lua_debug_current_depth(target);
    g_sess.step_L = target;
    clear_paused_thread();
    send_response(req, cJSON_CreateObject(), 1, NULL);
}

static void handle_step_out(lua_State *L, cJSON *req) {
    lua_State *target = step_target_L(L);
    g_sess.step = DAP_STEP_OUT;
    g_sess.step_depth = lua_debug_current_depth(target);
    g_sess.step_L = target;
    clear_paused_thread();
    send_response(req, cJSON_CreateObject(), 1, NULL);
}

static void handle_stack_trace(lua_State *L, cJSON *req) {
    cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    cJSON *tidj = args ? cJSON_GetObjectItemCaseSensitive(args, "threadId") : NULL;
    int tid = (tidj && cJSON_IsNumber(tidj)) ? (int)tidj->valuedouble : 1;
    lua_State *target = coro_registry_state_for(tid);
    cJSON *body;

    (void)L;
    if (!target) {
        send_response(req, NULL, 0, "unknown thread");
        return;
    }
    if (!dap_session_is_paused() || tid != dap_session_paused_thread_id()) {
        body = cJSON_CreateObject();
        if (body) {
            cJSON_AddArrayToObject(body, "stackFrames");
            cJSON_AddNumberToObject(body, "totalFrames", 0);
        }
        send_response(req, body, 1, NULL);
        return;
    }
    send_response(req, lua_debug_stack_frames(dap_session_paused_L()), 1, NULL);
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
    lua_State *target = (dap_session_is_paused() && dap_session_paused_L())
                            ? dap_session_paused_L()
                            : L;
    send_response(req, lua_debug_collect_variables(target, ref), 1, NULL);
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
    else if (strcmp(cmd, "attach") == 0)
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

int dap_session_is_paused(void) { return g_sess.paused; }

void dap_session_set_paused(int paused) { g_sess.paused = paused ? 1 : 0; }

void dap_session_set_paused_thread(lua_State *L, int thread_id) {
    g_sess.paused_L = L;
    g_sess.paused_thread_id = thread_id > 0 ? thread_id : 1;
}

int dap_session_paused_thread_id(void) {
    return g_sess.paused_thread_id > 0 ? g_sess.paused_thread_id : 1;
}

lua_State *dap_session_paused_L(void) { return g_sess.paused_L; }

int dap_session_step_mode(void) { return g_sess.step; }

int dap_session_step_depth(void) { return g_sess.step_depth; }

lua_State *dap_session_step_L(void) { return g_sess.step_L; }

void dap_session_clear_step(void) {
    g_sess.step = DAP_STEP_NONE;
    g_sess.step_depth = 0;
    g_sess.step_L = NULL;
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

int dap_session_send_stopped(const char *reason) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return -1;
    if (!cJSON_AddStringToObject(body, "reason", reason ? reason : "breakpoint") ||
        !cJSON_AddNumberToObject(body, "threadId",
                                 (double)dap_session_paused_thread_id()) ||
        !cJSON_AddBoolToObject(body, "allThreadsStopped", 0)) {
        cJSON_Delete(body);
        return -1;
    }
    return send_event("stopped", body);
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
    clear_paused_thread();
    g_sess.step = DAP_STEP_NONE;
    g_sess.step_depth = 0;
    g_sess.step_L = NULL;
    g_sess.client_open = 0;
    g_sess.seq = 0;

    lua_debug_clear_hook(L);
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
        clear_paused_thread();
        g_sess.configured = 1;
        return;
    }
    g_sess.dead = 1;
    g_sess.close_pending = 0;
    clear_paused_thread();
    g_sess.step = DAP_STEP_NONE;
    g_sess.step_depth = 0;
    g_sess.step_L = NULL;
    g_sess.configured = 1;

    /* Gold: reply + terminated while the client is still marked open, then
     * drop the hook and engine. CLOSE+MESSAGE batches drain before this. */
    conn_id = g_sess.dap_conn_id;
    can_send = (conn_id > 0 && g_sess.client_open);
    if (can_send) {
        if (disconnect_req)
            send_response(disconnect_req, cJSON_CreateObject(), 1, NULL);
        send_event("terminated", cJSON_CreateObject());
    }

    lua_debug_clear_hook(L);
    lua_debug_reset_var_maps(L);
    g_sess.hook_installed = 0;
    coro_registry_uninstall_wrappers(L);
    coro_registry_clear(L);
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

    if (!g_sess.listening || g_sess.dead) return 0;

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
                    return 0;
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
            return 0;
        }
        msg = dap_json_parse(json, jlen);
        free(json);
        if (!msg) {
            dap_session_reset_client(L, NULL);
            return 0;
        }
        dispatch(L, msg);
        cJSON_Delete(msg);
        if (g_sess.dead || !g_sess.client_open) break;
    }

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
    return 0;
}

int dap_session_start(lua_State *L, const char *host, int port, int wait) {
    char err[256];

    if (g_sess.listening || g_sess.recv_buf.data || g_sess.bp_files)
        dap_session_shutdown(L, NULL);
    bp_clear_all();
    dap_recv_buf_free(&g_sess.recv_buf);
    memset(&g_sess, 0, sizeof(g_sess));
    dap_recv_buf_init(&g_sess.recv_buf);
    g_sess.next_ref = 1000;
    g_sess.dap_conn_id = 0;

    if (as_net_init() != 0) return -1;
    if (as_listen(host, port, err, sizeof(err)) != 0) {
        fprintf(stderr, "[luadap] listen failed: %s\n", err);
        return -1;
    }
    g_sess.listening = 1;
    fprintf(stderr, "[luadap] listening on %s:%d\n", host, port);

    coro_registry_clear(L);
    coro_registry_track(L, L, "main");
    coro_registry_install_wrappers(L);

    if (wait) {
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
        }
    }
    return 0;
}
