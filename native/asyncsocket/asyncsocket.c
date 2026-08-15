#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "lua_compat.h"

#include "poll_loop.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#ifdef ASYNCSOCKET_STATIC
#define ASYNCSOCKET_API
#elif defined(_WIN32)
#define ASYNCSOCKET_API __declspec(dllexport)
#else
#define ASYNCSOCKET_API __attribute__((visibility("default")))
#endif

#define AS_SERVER_MT "asyncsocket.server"
#define AS_CONN_MT "asyncsocket.conn"

typedef struct {
    int alive; /* 1 while this object owns the current listen */
} as_server_ud;

typedef struct {
    int conn_id; /* 0 = invalidated after CLOSE */
} as_conn_ud;

static char k_conns;
static char k_server;

static void registry_get_conns(lua_State *L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &k_conns);
}

static void unreg_conn(lua_State *L, int conn_id) {
    registry_get_conns(L);
    lua_pushnil(L);
    lua_rawseti(L, -2, conn_id);
    lua_pop(L, 1);
}

static void set_current_server(lua_State *L, int idx) {
    lua_pushvalue(L, idx);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &k_server);
}

static void clear_current_server_if(lua_State *L, int idx) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &k_server);
    lua_pushvalue(L, idx);
    if (lua_rawequal(L, -1, -2)) {
        lua_pushnil(L);
        lua_rawsetp(L, LUA_REGISTRYINDEX, &k_server);
    }
    lua_pop(L, 2);
}

static as_server_ud *check_server(lua_State *L, int idx) {
    return (as_server_ud *)luaL_checkudata(L, idx, AS_SERVER_MT);
}

static as_conn_ud *check_conn(lua_State *L, int idx) {
    as_conn_ud *ud = (as_conn_ud *)luaL_checkudata(L, idx, AS_CONN_MT);
    if (ud->conn_id <= 0) {
        luaL_error(L, "asyncsocket connection is closed");
        return NULL;
    }
    return ud;
}

static void set_callback(lua_State *L, int ud_idx, const char *name) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getuservalue(L, ud_idx);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

static void push_conn(lua_State *L, int conn_id) {
    as_conn_ud *ud = (as_conn_ud *)lua_newuserdata(L, sizeof(as_conn_ud));
    ud->conn_id = conn_id;
    lua_newtable(L);
    lua_setuservalue(L, -2);
    luaL_setmetatable(L, AS_CONN_MT);

    registry_get_conns(L);
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, conn_id);
    lua_pop(L, 1);
}

static int server_on_accept(lua_State *L) {
    check_server(L, 1);
    set_callback(L, 1, "on_accept");
    return 0;
}

static int server_close(lua_State *L) {
    as_server_ud *ud = check_server(L, 1);
    if (!ud->alive) {
        return 0;
    }
    as_server_close();
    ud->alive = 0;
    clear_current_server_if(L, 1);
    return 0;
}

static int server_gc(lua_State *L) {
    as_server_ud *ud = check_server(L, 1);
    if (ud->alive) {
        as_server_close();
        ud->alive = 0;
        clear_current_server_if(L, 1);
    }
    return 0;
}

static int conn_on_open(lua_State *L) {
    check_conn(L, 1);
    set_callback(L, 1, "on_open");
    return 0;
}

static int conn_on_message(lua_State *L) {
    check_conn(L, 1);
    set_callback(L, 1, "on_message");
    return 0;
}

static int conn_on_close(lua_State *L) {
    check_conn(L, 1);
    set_callback(L, 1, "on_close");
    return 0;
}

static int conn_send(lua_State *L) {
    as_conn_ud *ud = check_conn(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (as_conn_send(ud->conn_id, data, len) != 0) {
        return luaL_error(L, "asyncsocket.send failed (no client or closed)");
    }
    return 0;
}

static int conn_close(lua_State *L) {
    as_conn_ud *ud = (as_conn_ud *)luaL_checkudata(L, 1, AS_CONN_MT);
    if (ud->conn_id <= 0) {
        return 0;
    }
    as_conn_close(ud->conn_id);
    return 0;
}

static int conn_gc(lua_State *L) {
    as_conn_ud *ud = (as_conn_ud *)luaL_checkudata(L, 1, AS_CONN_MT);
    if (ud->conn_id > 0) {
        as_conn_close(ud->conn_id);
        unreg_conn(L, ud->conn_id);
        ud->conn_id = 0;
    }
    return 0;
}

/* Returns 0 ok, -1 with Lua error object on stack. */
static int dispatch_accept(lua_State *L, int conn_id) {
    push_conn(L, conn_id);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &k_server);
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 2);
        return 0;
    }
    lua_getuservalue(L, -1);
    lua_getfield(L, -1, "on_accept");
    lua_remove(L, -2); /* uv */
    lua_remove(L, -2); /* server */
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2); /* fn, conn */
        return 0;
    }
    lua_pushvalue(L, -2); /* conn */
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        lua_remove(L, -2); /* conn */
        return -1;
    }
    lua_pop(L, 1); /* conn */
    return 0;
}

/* Returns 0 ok, -1 with Lua error object on stack. */
static int fire_conn_cb(lua_State *L, int conn_id, const char *name, int with_payload,
                        const char *payload, size_t len) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &k_conns);
    lua_rawgeti(L, -1, conn_id);
    lua_remove(L, -2);
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    lua_getuservalue(L, -1);
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return 0;
    }
    if (with_payload) {
        lua_pushlstring(L, payload ? payload : "", len);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_remove(L, -2);
            return -1;
        }
    } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        lua_remove(L, -2);
        return -1;
    }
    lua_pop(L, 1); /* udata */
    return 0;
}

static void invalidate_conn_ud(lua_State *L, int conn_id) {
    as_conn_ud *ud;
    registry_get_conns(L);
    lua_rawgeti(L, -1, conn_id);
    ud = (as_conn_ud *)luaL_testudata(L, -1, AS_CONN_MT);
    if (ud) {
        ud->conn_id = 0;
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_rawseti(L, -2, conn_id);
    lua_pop(L, 1);
}

static int dispatch_close(lua_State *L, int conn_id) {
    int rc = fire_conn_cb(L, conn_id, "on_close", 0, NULL, 0);
    invalidate_conn_ud(L, conn_id);
    return rc;
}

static int l_listen(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    char err[256];
    as_server_ud *ud;

    err[0] = '\0';
    if (as_listen(host, (int)port, err, sizeof(err)) != 0) {
        return luaL_error(L, "asyncsocket.listen failed: %s", err[0] ? err : "unknown");
    }

    ud = (as_server_ud *)lua_newuserdata(L, sizeof(as_server_ud));
    ud->alive = 1;
    lua_newtable(L);
    lua_setuservalue(L, -2);
    luaL_setmetatable(L, AS_SERVER_MT);
    set_current_server(L, -1);
    return 1;
}

static int l_connect(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    char err[256];
    int conn_id;

    err[0] = '\0';
    conn_id = as_connect(host, (int)port, err, sizeof(err));
    if (conn_id <= 0) {
        return luaL_error(L, "asyncsocket.connect failed: %s", err[0] ? err : "unknown");
    }
    push_conn(L, conn_id);
    return 1;
}

static int l_pump(lua_State *L) {
    size_t n = 0;
    size_t i;
    as_event *evs = as_take_events(&n);

    if (!evs || n == 0) {
        as_events_free(evs, n);
        return 0;
    }

    for (i = 0; i < n; i++) {
        int rc = 0;
        switch (evs[i].type) {
        case AS_EVT_ACCEPT:
            rc = dispatch_accept(L, evs[i].conn_id);
            break;
        case AS_EVT_OPEN:
            rc = fire_conn_cb(L, evs[i].conn_id, "on_open", 0, NULL, 0);
            break;
        case AS_EVT_MESSAGE:
            rc = fire_conn_cb(L, evs[i].conn_id, "on_message", 1, evs[i].payload, evs[i].len);
            break;
        case AS_EVT_CLOSE:
            rc = dispatch_close(L, evs[i].conn_id);
            break;
        default:
            break;
        }
        if (rc != 0) {
            as_events_free(evs, n);
            return lua_error(L);
        }
    }
    as_events_free(evs, n);
    return 0;
}

static int l_sleep(lua_State *L) {
    double sec = luaL_checknumber(L, 1);
    if (sec < 0) {
        sec = 0;
    }
#ifdef _WIN32
    Sleep((DWORD)(sec * 1000.0 + 0.5));
#else
    {
        struct timespec ts;
        ts.tv_sec = (time_t)sec;
        ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
        if (ts.tv_nsec < 0) {
            ts.tv_nsec = 0;
        }
        nanosleep(&ts, NULL);
    }
#endif
    return 0;
}

static const luaL_Reg server_methods[] = {
    {"on_accept", server_on_accept},
    {"close", server_close},
    {NULL, NULL}
};

static const luaL_Reg conn_methods[] = {
    {"on_open", conn_on_open},
    {"on_message", conn_on_message},
    {"on_close", conn_on_close},
    {"send", conn_send},
    {"close", conn_close},
    {NULL, NULL}
};

static void create_mt(lua_State *L, const char *name, lua_CFunction gc, const luaL_Reg *methods) {
    luaL_newmetatable(L, name);
    lua_pushcfunction(L, gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

ASYNCSOCKET_API int luaopen_asyncsocket(lua_State *L) {
    if (as_net_init() != 0) {
        return luaL_error(L, "asyncsocket: WSAStartup failed");
    }

    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &k_conns);

    create_mt(L, AS_SERVER_MT, server_gc, server_methods);
    create_mt(L, AS_CONN_MT, conn_gc, conn_methods);

    lua_newtable(L);
    lua_pushstring(L, "0.3.0");
    lua_setfield(L, -2, "_VERSION");
    lua_pushcfunction(L, l_listen);
    lua_setfield(L, -2, "listen");
    lua_pushcfunction(L, l_connect);
    lua_setfield(L, -2, "connect");
    lua_pushcfunction(L, l_pump);
    lua_setfield(L, -2, "pump");
    lua_pushcfunction(L, l_sleep);
    lua_setfield(L, -2, "sleep");
    return 1;
}
