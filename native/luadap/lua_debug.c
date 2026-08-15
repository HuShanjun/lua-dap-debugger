#include "lua_debug.h"
#include "dap_session.h"

#include <lauxlib.h>
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

#define MAX_USER_FRAMES 64
#define LOCALS_REF_BASE 100000
#define UPVALS_REF_BASE 200000
#define TABLE_REF_START 1000

typedef struct {
    int level;
} user_frame;

typedef struct {
    int dap_ref;
    int reg_ref;
    const void *ptr;
} var_entry;

static var_entry *g_vars;
static size_t g_var_n;
static size_t g_var_cap;
static int g_next_ref = TABLE_REF_START;

static int is_debugger_file(const char *norm) {
    if (!norm) return 1;
    return strstr(norm, "lua-runtime/debugger.lua") != NULL
        || strstr(norm, "lua-runtime/dkjson.lua") != NULL;
}

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

/* First @ user frame (skip debugger.lua / dkjson if present). */
static int find_user_line(lua_State *L, char **out_path, int *out_line) {
    lua_Debug ar;
    int level = 0;

    while (lua_getstack(L, level, &ar)) {
        if (lua_getinfo(L, "Sl", &ar) && ar.source && ar.source[0] == '@') {
            char *path = dap_session_normalize_path(ar.source);
            if (path && !is_debugger_file(path) && ar.currentline > 0) {
                *out_path = path;
                *out_line = ar.currentline;
                return 1;
            }
            free(path);
        }
        level++;
    }
    return 0;
}

/*
 * C line hook has no extra CallInfo (luaD_hook). pause_loop → update →
 * handler are C-to-C, so lua_getstack(0) is the interrupted user frame.
 * Keep @ sources only; frame 0 = closest to pause.
 */
static int walk_user_frames(lua_State *L, user_frame *frames) {
    lua_Debug ar;
    int level = 0;
    int n = 0;

    while (n < MAX_USER_FRAMES && lua_getstack(L, level, &ar)) {
        if (lua_getinfo(L, "S", &ar) && ar.source && ar.source[0] == '@') {
            char *path = dap_session_normalize_path(ar.source);
            if (path && !is_debugger_file(path)) {
                frames[n].level = level;
                n++;
            }
            free(path);
        }
        level++;
    }
    return n;
}

int lua_debug_current_depth(lua_State *L) {
    user_frame frames[MAX_USER_FRAMES];
    return walk_user_frames(L, frames);
}

void lua_debug_reset_var_maps(lua_State *L) {
    size_t i;
    if (L) {
        for (i = 0; i < g_var_n; i++)
            luaL_unref(L, LUA_REGISTRYINDEX, g_vars[i].reg_ref);
    }
    free(g_vars);
    g_vars = NULL;
    g_var_n = 0;
    g_var_cap = 0;
    g_next_ref = TABLE_REF_START;
}

static int alloc_ref(lua_State *L, int idx) {
    const void *ptr = lua_topointer(L, idx);
    var_entry *nv;
    var_entry *e;
    size_t i;
    size_t cap;

    for (i = 0; i < g_var_n; i++) {
        if (g_vars[i].ptr == ptr)
            return g_vars[i].dap_ref;
    }
    cap = g_var_cap;
    if (g_var_n >= cap) {
        cap = cap ? cap * 2 : 16;
        nv = (var_entry *)realloc(g_vars, cap * sizeof(*nv));
        if (!nv) return 0;
        g_vars = nv;
        g_var_cap = cap;
    }
    e = &g_vars[g_var_n++];
    e->dap_ref = g_next_ref++;
    e->ptr = ptr;
    lua_pushvalue(L, idx);
    e->reg_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return e->dap_ref;
}

static int push_ref_table(lua_State *L, int dap_ref) {
    size_t i;
    for (i = 0; i < g_var_n; i++) {
        if (g_vars[i].dap_ref == dap_ref) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, g_vars[i].reg_ref);
            return lua_istable(L, -1);
        }
    }
    return 0;
}

/* Lua 5.4 string.format("%q")-style quoting. */
static char *lua_quote_string(const char *s, size_t len) {
    size_t cap = len * 4 + 3;
    char *out;
    size_t w = 0;
    size_t i;

    if (!s) s = "";
    out = (char *)malloc(cap);
    if (!out) return NULL;
    out[w++] = '"';
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\' || c == '\n') {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c < 32) {
            int n = snprintf(out + w, cap - w, "\\%03d", (int)c);
            if (n > 0) w += (size_t)n;
        } else {
            out[w++] = (char)c;
        }
    }
    out[w++] = '"';
    out[w] = '\0';
    return out;
}

static char *value_to_name(lua_State *L, int idx) {
    size_t n = 0;
    const char *s;
    char *o;

    idx = lua_absindex(L, idx);
    s = luaL_tolstring(L, idx, &n);
    if (!s) {
        lua_pop(L, 1);
        return xstrdup("?");
    }
    o = (char *)malloc(n + 1);
    if (!o) {
        lua_pop(L, 1);
        return NULL;
    }
    memcpy(o, s, n);
    o[n] = '\0';
    lua_pop(L, 1);
    return o;
}

static int ancestor_has(const void **anc, int n, const void *p) {
    int i;
    if (!anc || !p) return 0;
    for (i = 0; i < n; i++) {
        if (anc[i] == p) return 1;
    }
    return 0;
}

static cJSON *format_var(lua_State *L, const char *name, int idx,
                         const void **anc, int nanc) {
    cJSON *o = cJSON_CreateObject();
    int t;

    if (!o) return NULL;
    idx = lua_absindex(L, idx);
    cJSON_AddStringToObject(o, "name", name ? name : "?");
    t = lua_type(L, idx);
    if (t == LUA_TTABLE) {
        const void *p = lua_topointer(L, idx);
        if (ancestor_has(anc, nanc, p)) {
            cJSON_AddStringToObject(o, "value", "table (circular)");
            cJSON_AddStringToObject(o, "type", "table");
            cJSON_AddNumberToObject(o, "variablesReference", 0);
        } else {
            int ref = alloc_ref(L, idx);
            cJSON_AddStringToObject(o, "value", "table");
            cJSON_AddStringToObject(o, "type", "table");
            cJSON_AddNumberToObject(o, "variablesReference", ref);
        }
    } else if (t == LUA_TSTRING) {
        size_t n = 0;
        const char *s = lua_tolstring(L, idx, &n);
        char *q = lua_quote_string(s, n);
        cJSON_AddStringToObject(o, "value", q ? q : "\"\"");
        cJSON_AddStringToObject(o, "type", "string");
        cJSON_AddNumberToObject(o, "variablesReference", 0);
        free(q);
    } else {
        size_t n = 0;
        const char *s = luaL_tolstring(L, idx, &n);
        cJSON_AddStringToObject(o, "value", s ? s : "");
        cJSON_AddStringToObject(o, "type", lua_typename(L, t));
        cJSON_AddNumberToObject(o, "variablesReference", 0);
        lua_pop(L, 1);
    }
    return o;
}

cJSON *lua_debug_stack_frames(lua_State *L) {
    user_frame frames[MAX_USER_FRAMES];
    int n = walk_user_frames(L, frames);
    cJSON *body = cJSON_CreateObject();
    cJSON *arr;
    int i;

    if (!body) return NULL;
    arr = cJSON_AddArrayToObject(body, "stackFrames");
    for (i = 0; i < n; i++) {
        lua_Debug ar;
        cJSON *fr;
        char *path;
        const char *base;

        if (!lua_getstack(L, frames[i].level, &ar) || !lua_getinfo(L, "Snl", &ar))
            continue;
        path = dap_session_normalize_path(ar.source);
        if (!path) continue;
        base = strrchr(path, '/');
        base = base ? base + 1 : path;
        fr = cJSON_CreateObject();
        if (fr) {
            cJSON *src = cJSON_CreateObject();
            cJSON_AddNumberToObject(fr, "id", i);
            cJSON_AddStringToObject(fr, "name", ar.name ? ar.name : "?");
            cJSON_AddNumberToObject(fr, "line",
                                    ar.currentline > 0 ? ar.currentline : 0);
            cJSON_AddNumberToObject(fr, "column", 0);
            if (src) {
                cJSON_AddStringToObject(src, "path", path);
                cJSON_AddStringToObject(src, "name", base);
                cJSON_AddItemToObject(fr, "source", src);
            }
            if (arr)
                cJSON_AddItemToArray(arr, fr);
            else
                cJSON_Delete(fr);
        }
        free(path);
    }
    cJSON_AddNumberToObject(body, "totalFrames", n);
    return body;
}

static cJSON *collect_locals(lua_State *L, int frame_id) {
    user_frame frames[MAX_USER_FRAMES];
    int n = walk_user_frames(L, frames);
    cJSON *arr = cJSON_CreateArray();
    lua_Debug ar;
    int i;

    if (!arr) return NULL;
    if (frame_id < 0 || frame_id >= n) return arr;
    if (!lua_getstack(L, frames[frame_id].level, &ar)) return arr;
    for (i = 1;; i++) {
        const char *name = lua_getlocal(L, &ar, i);
        if (!name) break;
        if (name[0] != '(') {
            cJSON *var = format_var(L, name, -1, NULL, 0);
            if (var) cJSON_AddItemToArray(arr, var);
        }
        lua_pop(L, 1);
    }
    return arr;
}

static cJSON *collect_upvalues(lua_State *L, int frame_id) {
    user_frame frames[MAX_USER_FRAMES];
    int n = walk_user_frames(L, frames);
    cJSON *arr = cJSON_CreateArray();
    lua_Debug ar;
    int i;

    if (!arr) return NULL;
    if (frame_id < 0 || frame_id >= n) return arr;
    if (!lua_getstack(L, frames[frame_id].level, &ar)) return arr;
    if (!lua_getinfo(L, "f", &ar)) return arr;
    for (i = 1;; i++) {
        const char *name = lua_getupvalue(L, -1, i);
        if (!name) break;
        {
            cJSON *var = format_var(L, name, -1, NULL, 0);
            if (var) cJSON_AddItemToArray(arr, var);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return arr;
}

typedef struct {
    cJSON *var;
    char *name;
} named_var;

static int cmp_named(const void *a, const void *b) {
    const char *na = ((const named_var *)a)->name;
    const char *nb = ((const named_var *)b)->name;
    if (!na) na = "";
    if (!nb) nb = "";
    return strcmp(na, nb);
}

static cJSON *collect_table(lua_State *L, int tbl_idx, const void **anc, int nanc) {
    const void *self = lua_topointer(L, tbl_idx);
    const void *next_anc[64];
    int next_n = 0;
    named_var *items = NULL;
    size_t nitems = 0;
    size_t cap = 0;
    cJSON *arr;
    size_t i;
    int abs_tbl;
    int found = 0;

    if (nanc > 62) nanc = 62;
    for (i = 0; i < (size_t)nanc; i++)
        next_anc[next_n++] = anc[i];
    for (i = 0; i < (size_t)next_n; i++) {
        if (next_anc[i] == self) {
            found = 1;
            break;
        }
    }
    if (!found && next_n < 64)
        next_anc[next_n++] = self;

    abs_tbl = lua_absindex(L, tbl_idx);
    lua_pushnil(L);
    while (lua_next(L, abs_tbl) != 0) {
        char *kname = value_to_name(L, -2);
        cJSON *var = format_var(L, kname ? kname : "?", -1, next_anc, next_n);
        if (var) {
            if (nitems >= cap) {
                size_t ncap = cap ? cap * 2 : 8;
                named_var *ni = (named_var *)realloc(items, ncap * sizeof(*ni));
                if (!ni) {
                    cJSON_Delete(var);
                    free(kname);
                    lua_pop(L, 1);
                    break;
                }
                items = ni;
                cap = ncap;
            }
            items[nitems].var = var;
            items[nitems].name = kname;
            nitems++;
        } else {
            free(kname);
        }
        lua_pop(L, 1);
    }

    if (nitems > 1)
        qsort(items, nitems, sizeof(*items), cmp_named);
    arr = cJSON_CreateArray();
    for (i = 0; i < nitems; i++) {
        if (arr)
            cJSON_AddItemToArray(arr, items[i].var);
        else
            cJSON_Delete(items[i].var);
        free(items[i].name);
    }
    free(items);
    return arr;
}

cJSON *lua_debug_collect_variables(lua_State *L, int ref) {
    cJSON *vars = NULL;
    cJSON *body;

    if (ref >= UPVALS_REF_BASE && ref < UPVALS_REF_BASE + LOCALS_REF_BASE)
        vars = collect_upvalues(L, ref - UPVALS_REF_BASE);
    else if (ref >= LOCALS_REF_BASE && ref < UPVALS_REF_BASE)
        vars = collect_locals(L, ref - LOCALS_REF_BASE);
    else if (push_ref_table(L, ref)) {
        const void *self = lua_topointer(L, -1);
        const void *anc[1];
        anc[0] = self;
        vars = collect_table(L, -1, anc, 1);
        lua_pop(L, 1);
    } else {
        vars = cJSON_CreateArray();
    }

    body = cJSON_CreateObject();
    if (!body) {
        cJSON_Delete(vars);
        return NULL;
    }
    if (!vars)
        vars = cJSON_CreateArray();
    if (vars)
        cJSON_AddItemToObject(body, "variables", vars);
    return body;
}

static void pause_loop(lua_State *L, const char *reason) {
    dap_session_set_paused(1);
    dap_session_reset_var_maps(L);
    if (dap_session_send_stopped(reason) != 0) {
        dap_session_shutdown(L, NULL);
        return;
    }
    while (dap_session_is_paused()) {
        if (dap_session_update(L) != 0) {
            dap_session_shutdown(L, NULL);
            break;
        }
        if (dap_session_is_dead())
            break;
        short_sleep();
    }
}

static void on_line_hook(lua_State *L, lua_Debug *ar) {
    char *path = NULL;
    int line = 0;

    (void)ar;
    if (dap_session_is_dead() || !dap_session_client_open())
        return;
    if (!find_user_line(L, &path, &line))
        return;
    if (dap_session_bp_should_stop(path, line)) {
        free(path);
        pause_loop(L, "breakpoint");
        return;
    }
    free(path);

    /* Gold on_line after BP: in → next user line; over when d <= step_depth;
     * out when d < step_depth. */
    {
        int mode = dap_session_step_mode();
        if (mode == DAP_STEP_IN) {
            dap_session_clear_step();
            pause_loop(L, "step");
            return;
        }
        if (mode == DAP_STEP_OVER) {
            if (lua_debug_current_depth(L) <= dap_session_step_depth()) {
                dap_session_clear_step();
                pause_loop(L, "step");
            }
            return;
        }
        if (mode == DAP_STEP_OUT) {
            if (lua_debug_current_depth(L) < dap_session_step_depth()) {
                dap_session_clear_step();
                pause_loop(L, "step");
            }
            return;
        }
    }
}

void lua_debug_install_hook(lua_State *L) {
    if (!L) return;
    lua_sethook(L, on_line_hook, LUA_MASKLINE, 0);
}

void lua_debug_clear_hook(lua_State *L) {
    if (!L) return;
    lua_sethook(L, NULL, 0, 0);
}
