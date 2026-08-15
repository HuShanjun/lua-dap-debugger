#include "lua_compat.h"

#if LUA_VERSION_NUM == 501

int lua_absindex(lua_State *L, int i) {
    if (i < 0 && i > LUA_REGISTRYINDEX)
        i += lua_gettop(L) + 1;
    return i;
}

int lua_rawgetp(lua_State *L, int idx, const void *p) {
    int abs_i = lua_absindex(L, idx);
    lua_pushlightuserdata(L, (void *)p);
    lua_rawget(L, abs_i);
    return lua_type(L, -1);
}

void lua_rawsetp(lua_State *L, int idx, const void *p) {
    int abs_i = lua_absindex(L, idx);
    luaL_checkstack(L, 1, "not enough stack slots");
    lua_pushlightuserdata(L, (void *)p);
    lua_insert(L, -2);
    lua_rawset(L, abs_i);
}

/* Adapted from Lua 5.2 / luasocket / Kepler */
void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup) {
    luaL_checkstack(L, nup + 1, "too many upvalues");
    for (; l->name != NULL; l++) {
        int i;
        lua_pushstring(L, l->name);
        for (i = 0; i < nup; i++)
            lua_pushvalue(L, -(nup + 1));
        lua_pushcclosure(L, l->func, nup);
        lua_settable(L, -(nup + 3));
    }
    lua_pop(L, nup);
}

const char *luaL_tolstring(lua_State *L, int idx, size_t *len) {
    if (!luaL_callmeta(L, idx, "__tostring")) {
        int t = lua_type(L, idx);
        switch (t) {
        case LUA_TNIL:
            lua_pushliteral(L, "nil");
            break;
        case LUA_TSTRING:
        case LUA_TNUMBER:
            lua_pushvalue(L, idx);
            break;
        case LUA_TBOOLEAN:
            if (lua_toboolean(L, idx))
                lua_pushliteral(L, "true");
            else
                lua_pushliteral(L, "false");
            break;
        default:
            lua_pushfstring(L, "%s: %p", lua_typename(L, t),
                            lua_topointer(L, idx));
            break;
        }
    }
    return lua_tolstring(L, -1, len);
}

void luaL_setmetatable(lua_State *L, const char *tname) {
    luaL_checkstack(L, 1, "not enough stack slots");
    luaL_getmetatable(L, tname);
    lua_setmetatable(L, -2);
}

#endif /* 501 */
