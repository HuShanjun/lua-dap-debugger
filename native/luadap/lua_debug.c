#include "lua_debug.h"

/* Stub line hook until Task 3 (breakpoints / pause). Still calls lua_sethook
 * so wait=false hosts that poll debug.gethook() can observe handshake done. */
static void stub_line_hook(lua_State *L, lua_Debug *ar) {
    (void)L;
    (void)ar;
}

void lua_debug_install_hook(lua_State *L) {
    if (!L) return;
    lua_sethook(L, stub_line_hook, LUA_MASKLINE, 0);
}

void lua_debug_clear_hook(lua_State *L) {
    if (!L) return;
    lua_sethook(L, NULL, 0, 0);
}
