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

/* Breakpoint map: 1 if path+line has a BP. Empty/missing condition is NULL;
 * caller evals a non-empty condition (failure → no hit). */
int dap_session_bp_should_stop(const char *norm_path, int line);

/* Stored condition for path+line, or NULL if missing/empty. Session-owned. */
const char *dap_session_bp_condition(const char *norm_path, int line);

/* Step mode: 0 none, 1 in, 2 over, 3 out (debugger.lua step / step_depth). */
enum {
    DAP_STEP_NONE = 0,
    DAP_STEP_IN = 1,
    DAP_STEP_OVER = 2,
    DAP_STEP_OUT = 3
};

int dap_session_is_dead(void);
int dap_session_client_open(void);
int dap_session_hooks_active(void);
int dap_session_is_paused(void); /* any thread in the paused set */
int dap_session_paused_contains(int thread_id);
int dap_session_is_L_paused(lua_State *L);
int dap_session_pause_enter(lua_State *L, int thread_id, const char *reason);
void dap_session_pause_wait(int thread_id);
void dap_session_pause_wait_idle(int thread_id);
int dap_session_resume_thread(int thread_id);
lua_State *dap_session_paused_L_for(int thread_id);
int dap_session_step_mode_of(lua_State *L);
int dap_session_step_depth_of(lua_State *L);
void dap_session_clear_step_of(lua_State *L);
void dap_session_reset_var_maps(lua_State *L);

#endif
