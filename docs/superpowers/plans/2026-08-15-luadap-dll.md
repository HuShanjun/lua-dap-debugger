# luadap.dll Encapsulation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a single `bin/luadap.dll` that embeds debugger/dkjson, statically links asyncsocket, and exports only `start(host, port, is_wait_connect)` and `update()`.

**Architecture:** CMake embeds `.lua` sources as C string blobs; `luaopen_luadap` preloads `asyncsocket` + scripts; thin C wrappers forward to existing `debugger.listen` / `debugger.update`. `listen` gains a wait flag for `is_wait_connect`.

**Tech Stack:** C11, Lua 5.4, existing asyncsocket static lib, CMake, Python DAP tests

**Spec:** `docs/superpowers/specs/2026-08-15-luadap-dll-design.md`

## Global Constraints

- Public Lua API of `luadap`: only `start` / `update` (plus optional `_VERSION`)
- `start(..., true)` blocks until DAP `configurationDone`
- `start(..., false)` returns immediately; handshake via later `update()`
- Do **not** rewrite DAP/hook in C; embed existing Lua
- Deploy without needing disk `debugger.lua` / `dkjson.lua` / `asyncsocket.dll`
- Link `liblua` like other modules; MSVC export `luaopen_luadap`
- Keep `script/lua-runtime/*.lua` as editable sources; rebuild DLL when they change
- English concise commits
- Default when `is_wait_connect` is nil/omitted: **true** (C `lua_isnoneornil` → wait)

---

## File Structure

| File | Responsibility |
|------|----------------|
| `script/lua-runtime/debugger.lua` | Add `listen(host, port, wait?)` |
| `native/asyncsocket/CMakeLists.txt` | STATIC lib for luadap (+ keep SHARED for smoke) |
| `native/asyncsocket/asyncsocket.c` | `ASYNCSOCKET_STATIC` API macro |
| `native/luadap/embed_lua.cmake` | Generate embedded C from `.lua` |
| `native/luadap/luadap.c` | `luaopen_luadap`, `start`, `update` |
| `native/luadap/CMakeLists.txt` | Build `luadap.dll` |
| `CMakeLists.txt` | `add_subdirectory(native/luadap)` |
| `main/main.cpp` | Use `require("luadap")` |
| `script/test/run_debugee_luadap.lua` | Test entry via luadap |
| `script/test/test_dap_luadap_handshake.py` | Handshake via luadap |
| `script/test/test_dap_luadap_nowait.py` | start(false) path |
| `README.md` | One-DLL integration |

---

### Task 1: `listen(host, port, wait?)` in debugger.lua

**Files:**
- Modify: `script/lua-runtime/debugger.lua`
- Test: `script/test/test_dap_handshake.py` (default wait=true)

**Interfaces:**
- Produces: `M.listen(host, port, wait)` — `wait` defaults to `true`
- When `wait=false`: wire sock/callbacks/coro and return; **install_hook** only after `configurationDone` (not at end of listen)

- [ ] **Step 1: Implement wait flag**

Refactor current `M.listen` (~line 643):

```lua
function M.listen(host, port, wait)
    if wait == nil then wait = true end
    host, port = env_host_port(host, port)
    state.host, state.port = host, port
    state.configured = false
    state.dead = false
    state.close_pending = false
    state.paused = false
    state.client_open = false
    state.recv_buf = ""
    state.seq = 0

    state.sock = asyncsocket.listen(host, port)
    state.reader_coro = coroutine.create(reader_main)
    -- ... same on_open / on_message / on_close as today ...

    if wait then
        while not state.configured do
            local ok = pcall(M.update)
            if not ok then
                shutdown_session()
                break
            end
            short_sleep()
        end
        if state.client_open and not state.dead then
            install_hook()
        end
    end
    return true
end
```

In `handle_configuration_done` after `state.configured = true`:

```lua
if state.client_open and not state.dead then
    install_hook()  -- idempotent if already installed
end
```

Ensure `install_hook` remains safe if called twice (already checks `hook_installed` or equivalent — add flag if missing).

- [ ] **Step 2: Regression**

```powershell
cd E:\demo\lua-dap-debugger\script\test
python test_dap_handshake.py
```

Expected: `handshake ok`

- [ ] **Step 3: Commit**

```powershell
git add script/lua-runtime/debugger.lua
git commit -m "feat: debugger.listen optional wait flag for luadap.start"
```

---

### Task 2: asyncsocket STATIC (+ keep SHARED)

**Files:**
- Modify: `native/asyncsocket/CMakeLists.txt`
- Modify: `native/asyncsocket/asyncsocket.c` (API macro for static)

**Interfaces:**
- Produces: `asyncsocket_static` target with `luaopen_asyncsocket`
- Keeps SHARED `asyncsocket` → `bin/asyncsocket.dll` for existing smoke

- [ ] **Step 1: Macro**

```c
#ifdef ASYNCSOCKET_STATIC
#define ASYNCSOCKET_API
#elif defined(_WIN32)
#define ASYNCSOCKET_API __declspec(dllexport)
#else
#define ASYNCSOCKET_API __attribute__((visibility("default")))
#endif
```

- [ ] **Step 2: CMake dual targets**

```cmake
add_library(asyncsocket_static STATIC asyncsocket.c poll_loop.c)
target_compile_definitions(asyncsocket_static PRIVATE ASYNCSOCKET_STATIC ...)
# same includes, link liblua, ws2_32 / Threads

add_library(asyncsocket SHARED asyncsocket.c poll_loop.c)
# existing SHARED settings
```

- [ ] **Step 3: Verify shared smoke**

```powershell
cmake --build build/msvc --config Debug --target asyncsocket
python script/test/test_asyncsocket_smoke.py
```

Expected: `asyncsocket smoke ok`

- [ ] **Step 4: Commit**

```powershell
git add native/asyncsocket
git commit -m "build: add asyncsocket_static for luadap linking"
```

---

### Task 3: CMake embed of debugger.lua + dkjson.lua

**Files:**
- Create: `native/luadap/embed_lua.cmake`
- Create: `native/luadap/CMakeLists.txt` (partial — embed rules)
- Generated (build tree): `embedded_debugger.c`, `embedded_dkjson.c`

**Interfaces:**
- Produces: `const char luadap_embedded_debugger[]`, `size_t luadap_embedded_debugger_len` (and dkjson equivalents)

- [ ] **Step 1: Write embed script**

PowerShell or CMake `file(READ)` + escape into C. Prefer `add_custom_command` invoking a small Python script `native/luadap/gen_embed.py`:

```python
# gen_embed.py input.lua output.c symbol_prefix
# writes: const char symbol_prefix[] = { ... bytes ... , 0}; const size_t symbol_prefix_len = N;
```

Using byte array `{ 'a','b', ...}` avoids escaping hell.

- [ ] **Step 2: Custom commands depend on the two .lua files**

- [ ] **Step 3: Commit** generator + CMake stub (not build-dir outputs)

```powershell
git add native/luadap/gen_embed.py native/luadap/embed_lua.cmake native/luadap/CMakeLists.txt
git commit -m "build: generate C blobs from debugger.lua and dkjson.lua"
```

---

### Task 4: luadap.c + luadap.dll

**Files:**
- Create: `native/luadap/luadap.c`
- Modify: `native/luadap/CMakeLists.txt`
- Modify: root `CMakeLists.txt`

**Interfaces:**
- Produces: `require("luadap")` → `{ start, update, _VERSION }`
- Consumes: embedded sources, `luaopen_asyncsocket`, debugger module

- [ ] **Step 1: Implement `luaopen_luadap`**

Outline:

1. `package.preload["asyncsocket"] = luaopen_asyncsocket`
2. Load dkjson buffer → `package.loaded["lua-runtime.dkjson"] = result`
3. Load debugger buffer → returns `M` → `luaL_ref` to registry as `ref_dbg`
4. Export table with `start` / `update`

```c
static int l_start(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    int wait = 1;
    if (!lua_isnoneornil(L, 3)) wait = lua_toboolean(L, 3);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_dbg);
    lua_getfield(L, -1, "listen");
    lua_pushstring(L, host);
    lua_pushinteger(L, port);
    lua_pushboolean(L, wait);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) return lua_error(L);
    return 0;
}

static int l_update(lua_State *L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref_dbg);
    lua_getfield(L, -1, "update");
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) return lua_error(L);
    return 0;
}
```

MSVC: `#define LUADAP_API __declspec(dllexport)` on `luaopen_luadap`.

- [ ] **Step 2: Link**

```cmake
add_library(luadap SHARED luadap.c ${EMBED_C_FILES})
target_link_libraries(luadap PRIVATE asyncsocket_static liblua)
# WIN32: ws2_32 if not propagated
set_target_properties(luadap PROPERTIES OUTPUT_NAME "luadap" PREFIX "")
```

- [ ] **Step 3: Build + require smoke**

```powershell
cmake --build build/msvc --config Debug --target luadap
bin\lua.exe -e "package.cpath='bin/?.dll;bin/Debug/?.dll;'..package.cpath; local d=require('luadap'); assert(d.start and d.update); print('luadap ok')"
```

Expected: `luadap ok`

- [ ] **Step 4: Commit**

```powershell
git add native/luadap CMakeLists.txt
git commit -m "feat: luadap.dll with embedded debugger and start/update API"
```

---

### Task 5: main.cpp + luadap handshake test + README

**Files:**
- Modify: `main/main.cpp`
- Create: `script/test/run_debugee_luadap.lua`
- Create: `script/test/test_dap_luadap_handshake.py`
- Modify: `README.md`

**Interfaces:**
- Host: `require("luadap"); start(..., true); update()`

- [ ] **Step 1: Switch main to luadap**

```cpp
auto launch = R"(
local dap = require("luadap")
local host = os.getenv("LUADAP_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("LUADAP_PORT") or "8172")
dap.start(host, port, true)
)";
// dbg_update = require("luadap")["update"]
```

- [ ] **Step 2: Debugee for tests**

`run_debugee_luadap.lua`: set `package.cpath` to `bin/?.dll` only; **do not** put `script/?.lua` on path (proves embed). `require("luadap"); start(..., true);` then run work()/sample body.

- [ ] **Step 3: Python handshake** mirroring existing DAP client against that debugee

- [ ] **Step 4: Run tests + build main**

```powershell
python script/test/test_dap_luadap_handshake.py
cmake --build build/msvc --config Debug --target main
```

- [ ] **Step 5: README one-DLL section + commit**

```powershell
git commit -am "feat: wire host to luadap and add embed handshake test"
```

---

### Task 6: nowait smoke + full regression + mark spec done

**Files:**
- Create: `script/test/test_dap_luadap_nowait.py` (+ tiny debugee if needed)
- Modify: `docs/superpowers/specs/2026-08-15-luadap-dll-design.md` status → 已实现

- [ ] **Step 1: nowait test** — `start(host, port, false)` then loop `update()` until handshake completes from Python

- [ ] **Step 2: Full regression**

```powershell
python script/test/test_asyncsocket_smoke.py
python script/test/test_dap_luadap_handshake.py
python script/test/test_dap_luadap_nowait.py
python script/test/test_dap_handshake.py
python script/test/test_dap_breakpoint.py
python script/test/test_dap_step.py
python script/test/test_dap_disconnect.py
python script/test/test_dap_partial_frame.py
```

- [ ] **Step 3: Commit**

```powershell
git commit -am "test: luadap nowait path; mark luadap design implemented"
```

---

## Plan Self-Review

| Spec requirement | Task |
|------------------|------|
| Single `luadap.dll` | 4 |
| Only `start` / `update` | 4 |
| wait → configurationDone | 1, 4 |
| wait=false | 1, 6 |
| Embed lua sources | 3 |
| Static asyncsocket | 2 |
| No disk runtime files | 5 |
| main.cpp | 5 |
| Tests | 5–6 |

No TBD. Nil third arg to `start` defaults to wait=true.
