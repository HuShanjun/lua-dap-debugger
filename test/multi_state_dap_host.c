/* Dual lua_State same-thread DAP host. Loads luadap from bin/ via package.cpath.
 * args: --port N [--mt] [--mismatch]
 * --mt is reserved (Task 7); this host only pumps both states on one thread. */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifndef LUA_OK
#define LUA_OK 0
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef LUADAP_BIN_DIR
#define LUADAP_BIN_DIR "."
#endif
#ifndef TEST_SCRIPT_DIR
#define TEST_SCRIPT_DIR "."
#endif

static void host_sleep(void) {
#ifdef _WIN32
    Sleep(10);
#else
    usleep(10000);
#endif
}

static void slash_copy(char *dst, size_t dstsz, const char *src) {
    size_t i;
    for (i = 0; i + 1 < dstsz && src[i]; i++)
        dst[i] = (src[i] == '\\') ? '/' : src[i];
    dst[i] = '\0';
}

static void set_package_paths(lua_State *L, const char *bin) {
    char cpath[1024];
#ifdef _WIN32
    snprintf(cpath, sizeof(cpath), "%s/?.dll", bin);
#else
    snprintf(cpath, sizeof(cpath), "%s/?.so;%s/?.dll", bin, bin);
#endif
    lua_getglobal(L, "package");
    lua_pushstring(L, cpath);
    lua_setfield(L, -2, "cpath");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);
}

static void prepare_state(lua_State *L, const char *bin, int port) {
    luaL_openlibs(L);
    set_package_paths(L, bin);
    lua_pushstring(L, bin);
    lua_setglobal(L, "BIN");
    lua_pushinteger(L, port);
    lua_setglobal(L, "PORT");
}

static int load_script(lua_State *L, const char *path) {
    if (luaL_dofile(L, path) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        return -1;
    }
    return 0;
}

static void call_named(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "%s: %s\n", name, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void call_update(lua_State *L) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "luadap");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        fprintf(stderr, "require luadap: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "update");
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "update: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void usage(void) {
    fprintf(stderr, "usage: multi_state_dap_host --port N [--mt] [--mismatch]\n");
}

int main(int argc, char **argv) {
    int port = 18210;
    int mismatch = 0;
    int i;
    char bin[512];
    char script_dir[512];
    char path_a[768];
    char path_b[768];
    lua_State *L_a;
    lua_State *L_b;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--mismatch") == 0) {
            mismatch = 1;
            continue;
        }
        if (strcmp(argv[i], "--mt") == 0) {
            fprintf(stderr, "multi-thread mode not implemented\n");
            return 1;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
        fprintf(stderr, "unknown arg: %s\n", argv[i]);
        usage();
        return 1;
    }
    if (port <= 0) {
        fprintf(stderr, "invalid --port\n");
        return 1;
    }

    slash_copy(bin, sizeof(bin), LUADAP_BIN_DIR);
    slash_copy(script_dir, sizeof(script_dir), TEST_SCRIPT_DIR);
    snprintf(path_a, sizeof(path_a), "%s/run_ms_a.lua", script_dir);
    snprintf(path_b, sizeof(path_b), "%s/run_ms_b.lua", script_dir);

    L_a = luaL_newstate();
    L_b = luaL_newstate();
    if (!L_a || !L_b) {
        fprintf(stderr, "luaL_newstate failed\n");
        if (L_a) lua_close(L_a);
        if (L_b) lua_close(L_b);
        return 1;
    }

    prepare_state(L_a, bin, port);
    if (load_script(L_a, path_a) != 0) {
        lua_close(L_a);
        lua_close(L_b);
        return 1;
    }
    printf("listening on 127.0.0.1:%d\n", port);
    fflush(stdout);

    prepare_state(L_b, bin, mismatch ? port + 1 : port);
    if (load_script(L_b, path_b) != 0) {
        printf("FAIL_JOIN\n");
        fflush(stdout);
        lua_close(L_a);
        lua_close(L_b);
        return 2;
    }

    for (;;) {
        call_named(L_a, "tick");
        call_update(L_a);
        call_named(L_b, "tick");
        call_update(L_b);
        host_sleep();
    }
}
