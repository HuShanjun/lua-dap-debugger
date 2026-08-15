#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "poll_loop.h"

#ifdef _WIN32
#define ASYNCSOCKET_API __declspec(dllexport)
#else
#define ASYNCSOCKET_API __attribute__((visibility("default")))
#endif

#define AS_UDATA_MT "asyncsocket"

static as_socket *g_impl = NULL;

static as_socket **check_ud(lua_State *L, int idx) {
    return (as_socket **)luaL_checkudata(L, idx, AS_UDATA_MT);
}

static as_socket *check_sock(lua_State *L, int idx) {
    as_socket **ud = check_ud(L, idx);
    if (!*ud) {
        luaL_error(L, "asyncsocket is closed");
        return NULL;
    }
    return *ud;
}

static void set_callback(lua_State *L, const char *name) {
    check_ud(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getiuservalue(L, 1, 1);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, name);
}

static int sock_on_open(lua_State *L) {
    set_callback(L, "on_open");
    return 0;
}

static int sock_on_message(lua_State *L) {
    set_callback(L, "on_message");
    return 0;
}

static int sock_on_close(lua_State *L) {
    set_callback(L, "on_close");
    return 0;
}

static int sock_send(lua_State *L) {
    as_socket *s = check_sock(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (as_socket_send(s, data, len) != 0) {
        return luaL_error(L, "asyncsocket.send failed (no client or closed)");
    }
    return 0;
}

static void clear_global_if(as_socket *s) {
    if (g_impl == s) {
        g_impl = NULL;
    }
}

static void registry_set_cb(lua_State *L, as_socket *s, int cb_idx) {
    lua_pushvalue(L, cb_idx);
    lua_rawsetp(L, LUA_REGISTRYINDEX, s);
}

static void registry_clear_cb(lua_State *L, as_socket *s) {
    lua_pushnil(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, s);
}

static const char *event_cb_name(as_event_type type) {
    switch (type) {
    case AS_EVT_OPEN:
        return "on_open";
    case AS_EVT_MESSAGE:
        return "on_message";
    case AS_EVT_CLOSE:
        return "on_close";
    default:
        return NULL;
    }
}

static int fire_events(lua_State *L, as_event *evs, size_t n, int cbtable) {
    size_t i;

    if (!evs || n == 0) {
        as_events_free(evs, n);
        return 0;
    }

    for (i = 0; i < n; i++) {
        const char *name = event_cb_name(evs[i].type);
        int nargs = 0;
        if (!name) {
            continue;
        }
        lua_getfield(L, cbtable, name);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        if (evs[i].type == AS_EVT_MESSAGE) {
            lua_pushlstring(L, evs[i].payload ? evs[i].payload : "", evs[i].len);
            nargs = 1;
        }
        if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
            as_events_free(evs, n);
            return lua_error(L);
        }
    }
    as_events_free(evs, n);
    return 0;
}

static void release_sock(lua_State *L, as_socket *s) {
    registry_clear_cb(L, s);
    clear_global_if(s);
    as_socket_destroy(s);
}

static int sock_close(lua_State *L) {
    as_socket **ud = check_ud(L, 1);
    as_socket *s = *ud;
    as_event *evs;
    size_t n = 0;
    int cbtable;

    if (!s) {
        return 0;
    }
    as_socket_stop(s);
    evs = as_socket_take_events(s, &n);
    lua_getiuservalue(L, 1, 1);
    cbtable = lua_gettop(L);
    *ud = NULL;
    release_sock(L, s);
    return fire_events(L, evs, n, cbtable);
}

static int sock_gc(lua_State *L) {
    as_socket **ud = check_ud(L, 1);
    as_socket *s = *ud;
    if (!s) {
        return 0;
    }
    *ud = NULL;
    release_sock(L, s);
    return 0;
}

static int l_listen(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    char err[256];
    as_socket *s;
    as_socket **ud;

    if (g_impl) {
        return luaL_error(L, "asyncsocket.listen: V1 allows only one listen");
    }
    err[0] = '\0';
    s = as_socket_listen(host, (int)port, err, sizeof(err));
    if (!s) {
        return luaL_error(L, "asyncsocket.listen failed: %s", err[0] ? err : "unknown");
    }

    ud = (as_socket **)lua_newuserdatauv(L, sizeof(as_socket *), 1);
    *ud = s;
    lua_newtable(L);
    registry_set_cb(L, s, -1);
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, AS_UDATA_MT);
    g_impl = s;
    return 1;
}

static int l_pump(lua_State *L) {
    as_event *evs = NULL;
    size_t n = 0;
    int cbtable;

    if (!g_impl) {
        return 0;
    }

    lua_rawgetp(L, LUA_REGISTRYINDEX, g_impl);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    cbtable = lua_gettop(L);
    evs = as_socket_take_events(g_impl, &n);
    return fire_events(L, evs, n, cbtable);
}

static const luaL_Reg sock_methods[] = {
    {"on_open", sock_on_open},
    {"on_message", sock_on_message},
    {"on_close", sock_on_close},
    {"send", sock_send},
    {"close", sock_close},
    {NULL, NULL}
};

ASYNCSOCKET_API int luaopen_asyncsocket(lua_State *L) {
    if (as_net_init() != 0) {
        return luaL_error(L, "asyncsocket: WSAStartup failed");
    }

    luaL_newmetatable(L, AS_UDATA_MT);
    lua_pushcfunction(L, sock_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, sock_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushstring(L, "0.1.0");
    lua_setfield(L, -2, "_VERSION");
    lua_pushcfunction(L, l_listen);
    lua_setfield(L, -2, "listen");
    lua_pushcfunction(L, l_pump);
    lua_setfield(L, -2, "pump");
    return 1;
}
