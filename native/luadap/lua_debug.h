#ifndef LUA_DEBUG_H
#define LUA_DEBUG_H

#include <lua.h>
#include <cJSON.h>

void lua_debug_install_hook(lua_State *L);
void lua_debug_clear_hook(lua_State *L);

/* Count @ user frames (skip debugger.lua / dkjson). Same baseline as gold
 * current_depth(): handlers and on_line share this number. */
int lua_debug_current_depth(lua_State *L);

/* Drop table refs (registry) and reset allocator to 1000. Call each stop. */
void lua_debug_reset_var_maps(lua_State *L);

/* DAP body: { stackFrames, totalFrames }. Caller owns / send_response frees. */
cJSON *lua_debug_stack_frames(lua_State *L);

/* DAP body: { variables }. ref: 100000+frame locals, 200000+frame upvalues,
 * else table object allocated this stop. */
cJSON *lua_debug_collect_variables(lua_State *L, int variables_reference);

#endif
