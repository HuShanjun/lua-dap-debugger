#include <lua.h>
#include <lauxlib.h>
#include "lua_compat.h"
#include "coro_registry.h"
#include "dap_session.h"

#ifdef LUADAP_STATIC
#define LUADAP_API
#elif defined(_WIN32)
#define LUADAP_API __declspec(dllexport)
#else
#define LUADAP_API __attribute__((visibility("default")))
#endif

#define LUADAP_VERSION "0.2.0"

static int l_start(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int wait = lua_isnoneornil(L, 3) ? 1 : lua_toboolean(L, 3);
    const char *name = lua_isnoneornil(L, 4) ? NULL : luaL_checkstring(L, 4);
    if (dap_session_start_ex(L, host, port, wait, name) != 0)
        return luaL_error(L, "luadap.start failed");
    return 0;
}

static int l_update(lua_State *L) {
    if (dap_session_update(L) != 0)
        return luaL_error(L, "luadap.update failed");
    return 0;
}

static int l_track(lua_State *L) {
    lua_State *co = lua_tothread(L, 1);
    const char *name = NULL;
    int id;
    if (!co) return luaL_error(L, "track: expected thread");
    if (!lua_isnoneornil(L, 2)) name = luaL_checkstring(L, 2);
    id = coro_registry_track(L, co, name);
    if (id == 0) return luaL_error(L, "track failed");
    lua_pushinteger(L, id);
    return 1;
}

LUADAP_API int luaopen_luadap(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_start);
    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_update);
    lua_setfield(L, -2, "update");
    lua_pushcfunction(L, l_track);
    lua_setfield(L, -2, "track");
    lua_pushstring(L, LUADAP_VERSION);
    lua_setfield(L, -2, "_VERSION");
    return 1;
}
