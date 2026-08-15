#include "coro_registry.h"
#include "dap_session.h"
#include "lua_debug.h"

#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    int reg_ref;
    lua_State *co;
    char name[64];
} coro_entry;

static coro_entry *g_entries;
static size_t g_n;
static size_t g_cap;
static int g_next_id = 2;
static int g_wrapped;
static lua_State *g_mainL;

static char key_orig_create;
static char key_orig_wrap;

static int entries_grow(void) {
    size_t cap = g_cap ? g_cap * 2 : 8;
    coro_entry *n = (coro_entry *)realloc(g_entries, cap * sizeof(*n));
    if (!n) return -1;
    g_entries = n;
    g_cap = cap;
    return 0;
}

static void remove_at(lua_State *mainL, size_t i) {
    luaL_unref(mainL, LUA_REGISTRYINDEX, g_entries[i].reg_ref);
    if (i + 1 < g_n)
        memmove(&g_entries[i], &g_entries[i + 1],
                (g_n - i - 1) * sizeof(g_entries[0]));
    g_n--;
}

static int is_main_thread(lua_State *L) {
    int main_th = lua_pushthread(L);
    lua_pop(L, 1);
    return main_th;
}

/* LUA_YIELD = suspended; error status = dead; LUA_OK + empty stack and no
 * frame = finished; LUA_OK with values or a frame = alive / running. */
static int coro_is_dead(lua_State *co) {
    int status;
    lua_Debug ar;

    if (!co) return 1;
    status = lua_status(co);
    if (status == LUA_YIELD) return 0;
    if (status != LUA_OK) return 1;
    if (lua_getstack(co, 0, &ar)) return 0;
    return lua_gettop(co) == 0;
}

static void set_name(coro_entry *e, const char *name_opt) {
    if (name_opt && name_opt[0])
        snprintf(e->name, sizeof(e->name), "%s", name_opt);
    else if (e->id == 1)
        snprintf(e->name, sizeof(e->name), "main");
    else
        snprintf(e->name, sizeof(e->name), "coro-%d", e->id);
}

int coro_registry_id_for(lua_State *co) {
    size_t i;
    if (!co) return 0;
    for (i = 0; i < g_n; i++) {
        if (g_entries[i].co == co)
            return g_entries[i].id;
    }
    return 0;
}

lua_State *coro_registry_state_for(int thread_id) {
    size_t i;
    for (i = 0; i < g_n; i++) {
        if (g_entries[i].id == thread_id)
            return g_entries[i].co;
    }
    return NULL;
}

void coro_registry_purge_dead(lua_State *mainL) {
    size_t i = 0;

    if (!mainL) mainL = g_mainL;
    if (!mainL) return;

    while (i < g_n) {
        coro_entry *e = &g_entries[i];
        lua_State *co;

        if (e->id == 1) {
            i++;
            continue;
        }
        lua_rawgeti(mainL, LUA_REGISTRYINDEX, e->reg_ref);
        if (!lua_isthread(mainL, -1)) {
            lua_pop(mainL, 1);
            remove_at(mainL, i);
            continue;
        }
        co = lua_tothread(mainL, -1);
        lua_pop(mainL, 1);
        if (coro_is_dead(co)) {
            lua_debug_clear_hook(co);
            remove_at(mainL, i);
            continue;
        }
        e->co = co;
        i++;
    }
}

int coro_registry_append_threads_json(cJSON *threads_array) {
    size_t i;

    if (!threads_array) return -1;
    if (g_mainL)
        coro_registry_purge_dead(g_mainL);
    for (i = 0; i < g_n; i++) {
        cJSON *th = cJSON_CreateObject();
        if (!th) return -1;
        if (!cJSON_AddNumberToObject(th, "id", (double)g_entries[i].id) ||
            !cJSON_AddStringToObject(th, "name", g_entries[i].name)) {
            cJSON_Delete(th);
            return -1;
        }
        cJSON_AddItemToArray(threads_array, th);
    }
    return 0;
}

int coro_registry_track(lua_State *mainL, lua_State *co, const char *name_opt) {
    int existing;
    coro_entry *e;
    int id;

    if (!mainL || !co) return 0;

    existing = coro_registry_id_for(co);
    if (existing) {
        size_t i;
        if (name_opt && name_opt[0]) {
            for (i = 0; i < g_n; i++) {
                if (g_entries[i].id == existing) {
                    snprintf(g_entries[i].name, sizeof(g_entries[i].name), "%s",
                             name_opt);
                    break;
                }
            }
        }
        return existing;
    }

    if (g_n >= g_cap && entries_grow() != 0) return 0;

    if (is_main_thread(co)) {
        id = 1;
        g_mainL = co;
    } else {
        id = g_next_id++;
        if (!g_mainL)
            g_mainL = mainL;
    }

    e = &g_entries[g_n];
    memset(e, 0, sizeof(*e));
    e->id = id;
    e->co = co;
    set_name(e, name_opt);

    lua_pushthread(co);
    if (co != mainL)
        lua_xmove(co, mainL, 1);
    e->reg_ref = luaL_ref(mainL, LUA_REGISTRYINDEX);
    g_n++;

    if (dap_session_hooks_active())
        lua_debug_install_hook(co);

    return id;
}

void coro_registry_clear(lua_State *mainL) {
    size_t i;

    if (!mainL) mainL = g_mainL;
    for (i = 0; i < g_n && mainL; i++) {
        lua_State *co = NULL;
        lua_rawgeti(mainL, LUA_REGISTRYINDEX, g_entries[i].reg_ref);
        if (lua_isthread(mainL, -1))
            co = lua_tothread(mainL, -1);
        lua_pop(mainL, 1);
        if (co)
            lua_debug_clear_hook(co);
        luaL_unref(mainL, LUA_REGISTRYINDEX, g_entries[i].reg_ref);
    }
    free(g_entries);
    g_entries = NULL;
    g_n = 0;
    g_cap = 0;
    g_next_id = 2;
    g_mainL = NULL;
}

static int l_wrapped_create(lua_State *L) {
    int n = lua_gettop(L);
    lua_State *co;

    lua_rawgetp(L, LUA_REGISTRYINDEX, &key_orig_create);
    lua_insert(L, 1);
    lua_call(L, n, 1);
    co = lua_tothread(L, -1);
    if (co)
        coro_registry_track(L, co, NULL);
    return 1;
}

static int l_wrapped_wrap(lua_State *L) {
    int n = lua_gettop(L);
    lua_State *co;

    lua_rawgetp(L, LUA_REGISTRYINDEX, &key_orig_wrap);
    lua_insert(L, 1);
    lua_call(L, n, 1);
    if (lua_isfunction(L, -1) && lua_getupvalue(L, -1, 1)) {
        co = lua_tothread(L, -1);
        if (co)
            coro_registry_track(L, co, NULL);
        lua_pop(L, 1);
    }
    return 1;
}

void coro_registry_install_wrappers(lua_State *mainL) {
    if (!mainL || g_wrapped) return;
    lua_getglobal(mainL, "coroutine");
    if (!lua_istable(mainL, -1)) {
        lua_pop(mainL, 1);
        return;
    }
    lua_getfield(mainL, -1, "create");
    lua_rawsetp(mainL, LUA_REGISTRYINDEX, &key_orig_create);
    lua_getfield(mainL, -1, "wrap");
    lua_rawsetp(mainL, LUA_REGISTRYINDEX, &key_orig_wrap);
    lua_pushcfunction(mainL, l_wrapped_create);
    lua_setfield(mainL, -2, "create");
    lua_pushcfunction(mainL, l_wrapped_wrap);
    lua_setfield(mainL, -2, "wrap");
    lua_pop(mainL, 1);
    g_wrapped = 1;
}

void coro_registry_uninstall_wrappers(lua_State *mainL) {
    if (!mainL || !g_wrapped) return;
    lua_getglobal(mainL, "coroutine");
    if (lua_istable(mainL, -1)) {
        lua_rawgetp(mainL, LUA_REGISTRYINDEX, &key_orig_create);
        if (!lua_isnil(mainL, -1))
            lua_setfield(mainL, -2, "create");
        else
            lua_pop(mainL, 1);
        lua_rawgetp(mainL, LUA_REGISTRYINDEX, &key_orig_wrap);
        if (!lua_isnil(mainL, -1))
            lua_setfield(mainL, -2, "wrap");
        else
            lua_pop(mainL, 1);
    }
    lua_pop(mainL, 1);
    lua_pushnil(mainL);
    lua_rawsetp(mainL, LUA_REGISTRYINDEX, &key_orig_create);
    lua_pushnil(mainL);
    lua_rawsetp(mainL, LUA_REGISTRYINDEX, &key_orig_wrap);
    g_wrapped = 0;
}
