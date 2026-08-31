#include "coro_registry.h"
#include "state_registry.h"

#include <cJSON.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

static int g_fails;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_fails++;
    }
}

static const char *json_name_for_id(cJSON *arr, int id) {
    int i, n = cJSON_GetArraySize(arr);
    for (i = 0; i < n; i++) {
        cJSON *th = cJSON_GetArrayItem(arr, i);
        cJSON *idj = cJSON_GetObjectItemCaseSensitive(th, "id");
        cJSON *nj = cJSON_GetObjectItemCaseSensitive(th, "name");
        if (idj && (int)idj->valuedouble == id)
            return (nj && cJSON_IsString(nj) && nj->valuestring) ? nj->valuestring
                                                                : "";
    }
    return NULL;
}

int main(void) {
    lua_State *a = luaL_newstate();
    lua_State *b = luaL_newstate();
    lua_State *co = NULL;
    int id_a, id_b, id_co;
    cJSON *arr;
    const char *nm;

    if (!a || !b)
        return 1;
    luaL_openlibs(a);
    luaL_openlibs(b);

    expect(state_registry_add(a, NULL) == 1, "first state id 1");
    id_a = coro_registry_track(a, a, NULL);
    expect(id_a == 1, "first main threadId=1");

    coro_registry_install_wrappers(a);
    if (luaL_dostring(a, "co = coroutine.create(function() end)") != 0) {
        fprintf(stderr, "lua: %s\n", lua_tostring(a, -1));
        return 1;
    }
    lua_getglobal(a, "co");
    co = lua_tothread(a, -1);
    lua_pop(a, 1);
    id_co = coro_registry_id_for(co);
    expect(id_co >= 2, "wrapped coro tracked");

    arr = cJSON_CreateArray();
    expect(arr && coro_registry_append_threads_json(arr) == 0, "append single");
    nm = json_name_for_id(arr, 1);
    expect(nm && strcmp(nm, "main/main") == 0, "alone main name is main/main");
    nm = json_name_for_id(arr, id_co);
    expect(nm && strstr(nm, "main/coro-") == nm, "alone coro is main/coro-N");
    cJSON_Delete(arr);

    expect(state_registry_add(b, "ui") >= 2, "second state added");
    id_b = coro_registry_track(b, b, NULL);
    expect(id_b != 0, "second main tracked");
    expect(id_b != 1, "second main must not steal threadId=1");
    expect(id_b != id_a, "thread ids unique");
    expect(coro_registry_id_for(a) == 1, "first main still id 1");
    expect(coro_registry_id_for(b) == id_b, "second main lookup");

    arr = cJSON_CreateArray();
    expect(arr && coro_registry_append_threads_json(arr) == 0, "append multi");
    nm = json_name_for_id(arr, 1);
    expect(nm && strcmp(nm, "main/main") == 0, "multi first main is main/main");
    nm = json_name_for_id(arr, id_b);
    expect(nm && strcmp(nm, "ui/main") == 0, "multi second main is ui/main");
    nm = json_name_for_id(arr, id_co);
    expect(nm && strstr(nm, "main/") == nm, "multi coro is state/coro");
    cJSON_Delete(arr);

    coro_registry_uninstall_wrappers(a);
    coro_registry_clear(a);
    coro_registry_clear(b);
    state_registry_clear();
    lua_close(a);
    lua_close(b);

    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("coro_registry multi-main ok\n");
    return 0;
}
