#ifndef DAP_SESSION_H
#define DAP_SESSION_H

#include <lua.h>
#include <cJSON.h>

typedef struct dap_session dap_session;

dap_session *dap_session_get(void); /* single global */
int dap_session_start(lua_State *L, const char *host, int port, int wait);
int dap_session_update(lua_State *L);
void dap_session_shutdown(lua_State *L, cJSON *disconnect_req /* nullable */);

#endif
