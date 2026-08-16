#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "lua_compat.h"
#include "dap_session.h"

extern int luaopen_luadap(lua_State *L);

#if LUA_VERSION_NUM < 502
static void runner_requiref(lua_State *L, const char *modname,
                            lua_CFunction openf, int glb) {
    lua_pushcfunction(L, openf);
    lua_pushstring(L, modname);
    lua_call(L, 1, 1);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loaded");
    lua_remove(L, -2);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, modname);
    lua_pop(L, 1);
    if (glb) {
        lua_pushvalue(L, -1);
        lua_setglobal(L, modname);
    }
}
#define luaL_requiref runner_requiref
#endif

static void usage(void) {
    fprintf(stderr,
            "usage: lua-runner [--host HOST] [--port PORT] [--] "
            "<program.lua> [script_args...]\n");
}

static void runner_sleep(void) {
#ifdef _WIN32
    Sleep(10);
#else
    usleep(10000);
#endif
}

int main(int argc, char **argv) {
    const char *host = NULL;
    const char *port_str = NULL;
    const char *program;
    int script_i;
    int i;
    int port;
    lua_State *L;
    int has_update;
    int n;

    i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
            i++;
            continue;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port_str = argv[++i];
            i++;
            continue;
        }
        break;
    }

    if (i >= argc) {
        usage();
        return 1;
    }
    program = argv[i];
    script_i = i;

    if (!host) {
        host = getenv("LUADAP_HOST");
        if (!host)
            host = "127.0.0.1";
    }
    if (!port_str) {
        port_str = getenv("LUADAP_PORT");
        if (!port_str)
            port_str = "8172";
    }
    port = atoi(port_str);

    L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "luaL_newstate failed\n");
        return 1;
    }
    luaL_openlibs(L);
    luaL_requiref(L, "luadap", luaopen_luadap, 1);
    lua_pop(L, 1);

    lua_newtable(L);
    for (i = script_i; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - script_i);
    }
    lua_setglobal(L, "arg");

    lua_getglobal(L, "luadap");
    lua_getfield(L, -1, "start");
    lua_pushstring(L, host);
    lua_pushinteger(L, port);
    lua_pushboolean(L, 1);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    lua_pop(L, 1); /* luadap table */

    printf("LISTEN_DONE\n");
    fflush(stdout);

    if (luaL_dofile(L, program) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    lua_settop(L, 0);

    lua_getglobal(L, "update");
    has_update = lua_isfunction(L, -1);
    if (!has_update)
        lua_pop(L, 1);

    n = 0;
    for (;;) {
        if (has_update) {
            lua_pushvalue(L, -1);
            lua_pushinteger(L, ++n);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                fprintf(stderr, "%s\n", lua_tostring(L, -1));
                break;
            }
        }
        lua_getglobal(L, "luadap");
        lua_getfield(L, -1, "update");
        lua_remove(L, -2);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            fprintf(stderr, "%s\n", lua_tostring(L, -1));
            break;
        }
        /* Hosts keep listening after soft-reset; this CLI exits when VS Code
         * disconnects (start already completed, client gone). */
        if (!dap_session_client_open())
            break;
        runner_sleep();
    }

    if (has_update)
        lua_pop(L, 1);
    lua_close(L);
    return 0;
}
