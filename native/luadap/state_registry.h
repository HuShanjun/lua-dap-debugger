#ifndef STATE_REGISTRY_H
#define STATE_REGISTRY_H

#include <lua.h>

int state_registry_count(void);
int state_registry_add(lua_State *mainL, const char *name_opt); /* id>=1 or 0 */
int state_registry_has(lua_State *mainL);
const char *state_registry_name(lua_State *mainL);
void state_registry_clear(void);

#endif
