#ifndef CORO_REGISTRY_H
#define CORO_REGISTRY_H

#include <lua.h>
#include <cJSON.h>

void coro_registry_clear(lua_State *mainL);
int coro_registry_track(lua_State *mainL, lua_State *co, const char *name_opt);
int coro_registry_id_for(lua_State *co);
lua_State *coro_registry_state_for(int thread_id);
void coro_registry_purge_dead(lua_State *mainL);
int coro_registry_append_threads_json(cJSON *threads_array);
void coro_registry_install_wrappers(lua_State *mainL);
void coro_registry_uninstall_wrappers(lua_State *mainL);
void coro_registry_install_hooks_all(void);

#endif
