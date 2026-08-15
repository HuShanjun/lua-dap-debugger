#ifndef LUA_DEBUG_H
#define LUA_DEBUG_H

#include <lua.h>

void lua_debug_install_hook(lua_State *L);
void lua_debug_clear_hook(lua_State *L);

#endif
