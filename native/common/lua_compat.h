#ifndef LUA_COMPAT_H
#define LUA_COMPAT_H

#include <lua.h>
#include <lauxlib.h>

#if LUA_VERSION_NUM < 501 || LUA_VERSION_NUM > 504
#error "lua_compat: unsupported Lua version"
#endif

#if LUA_VERSION_NUM == 501
#ifndef LUA_OK
#define LUA_OK 0
#endif
#ifndef lua_pushglobaltable
#define lua_pushglobaltable(L) lua_pushvalue((L), LUA_GLOBALSINDEX)
#endif
/* luaL_checkinteger is a real function in Lua 5.1 lauxlib; do not map it
 * to luaL_checkint (that macro casts to int and would truncate). */
/* Declarations — implemented in lua_compat.c */
#ifndef lua_absindex
int lua_absindex(lua_State *L, int i);
#endif
#ifndef lua_rawgetp
int lua_rawgetp(lua_State *L, int idx, const void *p);
#endif
#ifndef lua_rawsetp
void lua_rawsetp(lua_State *L, int idx, const void *p);
#endif
#ifndef luaL_setfuncs
void luaL_setfuncs(lua_State *L, const luaL_Reg *l, int nup);
#endif
#ifndef luaL_tolstring
const char *luaL_tolstring(lua_State *L, int idx, size_t *len);
#endif
#ifndef luaL_setmetatable
void luaL_setmetatable(lua_State *L, const char *tname);
#endif
#ifndef luaL_testudata
void *luaL_testudata(lua_State *L, int ud, const char *tname);
#endif
/* uservalue ↔ fenv (table only) */
#ifndef lua_getuservalue
#define lua_getuservalue(L, i) (lua_getfenv((L), (i)))
#endif
#ifndef lua_setuservalue
#define lua_setuservalue(L, i) \
  (luaL_checktype((L), -1, LUA_TTABLE), lua_setfenv((L), (i)))
#endif
#endif /* 501 */

#if LUA_VERSION_NUM == 502
/* luaL_tolstring exists in 5.2; rawgetp exists. Nothing required unless
 * return-type wrappers needed — keep empty if build is clean. */
#ifndef luaL_tolstring
const char *luaL_tolstring(lua_State *L, int idx, size_t *len);
#endif
#endif

#endif /* LUA_COMPAT_H */
