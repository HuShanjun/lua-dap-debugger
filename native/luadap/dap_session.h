#ifndef DAP_SESSION_H
#define DAP_SESSION_H

#include <lua.h>
#include <cJSON.h>

typedef struct dap_session dap_session;

dap_session *dap_session_get(void); /* single global */
int dap_session_start(lua_State *L, const char *host, int port, int wait);
int dap_session_update(lua_State *L);
void dap_session_shutdown(lua_State *L, cJSON *disconnect_req /* nullable */);

/* Path normalize (debugger.lua): strip @, \→/, lowercase drive, strip trailing /.
 * Returns malloc'd string; caller frees. */
char *dap_session_normalize_path(const char *path);

/* Breakpoint map: 1 if path+line has a BP. Empty/missing condition counts as hit
 * (condition eval is Task 6). */
int dap_session_bp_should_stop(const char *norm_path, int line);

int dap_session_is_dead(void);
int dap_session_client_open(void);
int dap_session_is_paused(void);
void dap_session_set_paused(int paused);
void dap_session_clear_step(void);
void dap_session_reset_var_maps(void);
int dap_session_send_stopped(const char *reason);

#endif
