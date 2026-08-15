#include "lua_debug.h"
#include "dap_session.h"

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
static void short_sleep(void) { Sleep(1); }
#else
#include <time.h>
static void short_sleep(void) {
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
}
#endif

static int is_debugger_file(const char *norm) {
    if (!norm) return 1;
    return strstr(norm, "lua-runtime/debugger.lua") != NULL
        || strstr(norm, "lua-runtime/dkjson.lua") != NULL;
}

/* First @ user frame (skip debugger.lua / dkjson if present). */
static int find_user_line(lua_State *L, char **out_path, int *out_line) {
    lua_Debug ar;
    int level = 0;

    while (lua_getstack(L, level, &ar)) {
        if (lua_getinfo(L, "Sl", &ar) && ar.source && ar.source[0] == '@') {
            char *path = dap_session_normalize_path(ar.source);
            if (path && !is_debugger_file(path) && ar.currentline > 0) {
                *out_path = path;
                *out_line = ar.currentline;
                return 1;
            }
            free(path);
        }
        level++;
    }
    return 0;
}

static void pause_loop(lua_State *L, const char *reason) {
    dap_session_set_paused(1);
    dap_session_reset_var_maps();
    if (dap_session_send_stopped(reason) != 0) {
        dap_session_shutdown(L, NULL);
        return;
    }
    while (dap_session_is_paused()) {
        if (dap_session_update(L) != 0) {
            dap_session_shutdown(L, NULL);
            break;
        }
        if (dap_session_is_dead())
            break;
        short_sleep();
    }
}

static void on_line_hook(lua_State *L, lua_Debug *ar) {
    char *path = NULL;
    int line = 0;

    (void)ar;
    if (dap_session_is_dead() || !dap_session_client_open())
        return;
    if (!find_user_line(L, &path, &line))
        return;
    if (dap_session_bp_should_stop(path, line)) {
        free(path);
        pause_loop(L, "breakpoint");
        return;
    }
    free(path);
}

void lua_debug_install_hook(lua_State *L) {
    if (!L) return;
    lua_sethook(L, on_line_hook, LUA_MASKLINE, 0);
}

void lua_debug_clear_hook(lua_State *L) {
    if (!L) return;
    lua_sethook(L, NULL, 0, 0);
}
