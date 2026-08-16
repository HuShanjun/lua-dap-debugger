#ifndef LUA_DEBUG_H
#define LUA_DEBUG_H

#include <lua.h>
#include <cJSON.h>

void lua_debug_install_hook(lua_State *L);
void lua_debug_clear_hook(lua_State *L);

/* Count @ user frames (skip debugger.lua / dkjson). Same baseline as gold
 * current_depth(): handlers and on_line share this number. */
int lua_debug_current_depth(lua_State *L);

/* Unref registry entries owned by L only; leave sibling-state maps intact.
 * Caller must hold session_mutex (or be the only thread). */
void lua_debug_reset_var_maps(lua_State *L);

/* Packed frameId = threadId * stride + index so scopes/variables/evaluate
 * can recover the owning DAP thread without a threadId argument. */
int lua_debug_pack_frame_id(int thread_id, int index);
int lua_debug_unpack_frame_index(int frame_id);
int lua_debug_unpack_frame_tid(int frame_id);

/* Owner L of a table variablesReference, or NULL. Caller holds session_mutex. */
lua_State *lua_debug_var_owner(int dap_ref);

/* DAP threadId encoded in a locals/upvalues/table variablesReference, or 0.
 * Caller holds session_mutex. */
int lua_debug_tid_from_varref(int variables_reference);

/* DAP body: { stackFrames, totalFrames }. frame ids are packed with thread_id.
 * Caller owns / send_response frees. */
cJSON *lua_debug_stack_frames(lua_State *L, int thread_id);

/* DAP body: { variables }. ref: 100000+packed-frame locals, 200000+packed-frame
 * upvalues, else table object allocated this stop. */
cJSON *lua_debug_collect_variables(lua_State *L, int variables_reference);

/* DAP evaluate body { result, type, variablesReference }. Failure: NULL and
 * *err is malloc'd (caller frees). watch/hover/other → return (expr). repl →
 * return (expr) or statement chunk; assignments write back via __newindex.
 * frame_id is the packed stack frame id. */
cJSON *lua_debug_evaluate(lua_State *L, const char *expression, int frame_id,
                          const char *context, char **err);

#endif
