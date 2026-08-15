#include <lua.h>
#include <lauxlib.h>
#include "dap_session.h"

#ifdef _WIN32
#define LUADAP_API __declspec(dllexport)
#else
#define LUADAP_API __attribute__((visibility("default")))
#endif

#define LUADAP_VERSION "0.2.0"

static int l_start(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int wait = 1;
    if (!lua_isnoneornil(L, 3)) wait = lua_toboolean(L, 3);
    if (dap_session_start(L, host, port, wait) != 0)
        return luaL_error(L, "luadap.start failed");
    return 0;
}

static int l_update(lua_State *L) {
    if (dap_session_update(L) != 0)
        return luaL_error(L, "luadap.update failed");
    return 0;
}

LUADAP_API int luaopen_luadap(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_start);
    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_update);
    lua_setfield(L, -2, "update");
    lua_pushstring(L, LUADAP_VERSION);
    lua_setfield(L, -2, "_VERSION");
    return 1;
}
