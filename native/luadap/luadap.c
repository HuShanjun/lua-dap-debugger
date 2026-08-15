#include <stddef.h>

#include <lauxlib.h>
#include <lua.h>

#ifdef _WIN32
#define LUADAP_API __declspec(dllexport)
#else
#define LUADAP_API __attribute__((visibility("default")))
#endif

#define LUADAP_VERSION "0.1.0"

extern int luaopen_asyncsocket(lua_State *L);
extern const char luadap_embedded_debugger[];
extern const size_t luadap_embedded_debugger_len;
extern const char luadap_embedded_dkjson[];
extern const size_t luadap_embedded_dkjson_len;

static int ref_dbg = LUA_NOREF;

static void load_embedded(lua_State *L, const char *buf, size_t len,
                          const char *name) {
    if (luaL_loadbuffer(L, buf, len, name) != LUA_OK) {
        lua_error(L);
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        lua_error(L);
    }
}

static int l_start(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    int wait = 1;
    if (!lua_isnoneornil(L, 3)) {
        wait = lua_toboolean(L, 3);
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_dbg);
    lua_getfield(L, -1, "listen");
    lua_pushstring(L, host);
    lua_pushinteger(L, port);
    lua_pushboolean(L, wait);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        return lua_error(L);
    }
    lua_pop(L, 1);
    return 0;
}

static int l_update(lua_State *L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_dbg);
    lua_getfield(L, -1, "update");
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        return lua_error(L);
    }
    lua_pop(L, 1);
    return 0;
}

LUADAP_API int luaopen_luadap(lua_State *L) {
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, luaopen_asyncsocket);
    lua_setfield(L, -2, "asyncsocket");
    lua_pop(L, 2);

    load_embedded(L, luadap_embedded_dkjson, luadap_embedded_dkjson_len,
                  "lua-runtime.dkjson");
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "lua-runtime.dkjson");
    lua_pop(L, 3);

    load_embedded(L, luadap_embedded_debugger, luadap_embedded_debugger_len,
                  "lua-runtime.debugger");
    if (!lua_istable(L, -1)) {
        return luaL_error(L, "embedded debugger did not return a table");
    }
    ref_dbg = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);
    lua_pushcfunction(L, l_start);
    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_update);
    lua_setfield(L, -2, "update");
    lua_pushstring(L, LUADAP_VERSION);
    lua_setfield(L, -2, "_VERSION");
    return 1;
}
