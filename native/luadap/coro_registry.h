#ifndef CORO_REGISTRY_H
#define CORO_REGISTRY_H

#include <lua.h>
#include <cJSON.h>

/* All APIs require the caller to hold session_mutex, except the
 * coroutine.create/wrap wrappers which take the mutex themselves. */
void coro_registry_clear(lua_State *mainL);
/* Drop C bookkeeping with no lua_* (fresh listen / leftover session). */
void coro_registry_reset(void);
int coro_registry_track(lua_State *mainL, lua_State *co, const char *name_opt);
int coro_registry_id_for(lua_State *co);
lua_State *coro_registry_state_for(int thread_id);
/* Purge dead coros whose mainL belongs to this updater. Never lua_* a sibling. */
void coro_registry_purge_dead(lua_State *mainL);
int coro_registry_append_threads_json(cJSON *threads_array);
void coro_registry_install_wrappers(lua_State *mainL);
void coro_registry_uninstall_wrappers(lua_State *mainL);
void coro_registry_install_hooks_owned(lua_State *owner);
void coro_registry_clear_hooks_owned(lua_State *owner);

#endif
