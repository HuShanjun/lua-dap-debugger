#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#ifdef _WIN32
#define ASYNCSOCKET_API __declspec(dllexport)
#else
#define ASYNCSOCKET_API __attribute__((visibility("default")))
#endif

ASYNCSOCKET_API int luaopen_asyncsocket(lua_State *L) {
    lua_newtable(L);
    lua_pushstring(L, "0.1.0");
    lua_setfield(L, -2, "_VERSION");
    return 1;
}
