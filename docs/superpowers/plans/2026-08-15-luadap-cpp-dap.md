# luadap C++ DAP Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace embedded `debugger.lua`/`dkjson` inside `luadap.dll` with a layered C/C++ DAP implementation that keeps `start`/`update`, reuses `asyncsocket` C API, and passes the existing Python DAP regression suite.

**Architecture:** `luadap` Lua facade → `dap_session` (protocol + state) → `dap_framing` + `dap_json` (cJSON) + `lua_debug` (`lua_sethook`, pause, variables). Transport via `as_socket_*` from `poll_loop.h` (static link). No reader coroutine; host-thread `update` pumps events and dispatches frames.

**Tech Stack:** C11/C++ (match repo), Lua 5.4 C API, cJSON (vendor), existing `asyncsocket_static`, CMake, Python DAP tests

**Spec:** `docs/superpowers/specs/2026-08-15-luadap-cpp-dap-design.md`

## Global Constraints

- Public Lua API: only `start` / `update` (optional `_VERSION`)
- `start(..., true)` blocks until DAP `configurationDone`; omitted/nil wait → **true**
- `start(..., false)` returns immediately; handshake via later `update()`
- **No** embedded Lua strings; delete `gen_embed.py` / `embed_lua.cmake` product path
- Feature parity with `script/lua-runtime/debugger.lua` (reference only; not loaded at runtime)
- Static-link `asyncsocket`; call `poll_loop.h` directly (do not `require("asyncsocket")` from Lua)
- JSON: **cJSON** under `3rd/cJSON/`
- Single global session (V1)
- Hook installed only on host thread (`update` / wait-loop exit), never from a wrong thread
- English concise commits
- Windows primary (`Sleep` for short wait); keep code portable where cheap

---

## File Structure

| File | Responsibility |
|------|----------------|
| `3rd/cJSON/cJSON.c`, `cJSON.h` | Vendored cJSON |
| `native/luadap/dap_framing.h/.c` | `Content-Length` buffer append / try_parse |
| `native/luadap/dap_json.h/.c` | Encode/decode helpers around cJSON |
| `native/luadap/dap_session.h/.c` | Session state, send, dispatch, start/update/shutdown |
| `native/luadap/lua_debug.h/.c` | Hook, breakpoints, step, pause_loop, stack/vars |
| `native/luadap/luadap.c` | `luaopen_luadap`, Lua `start`/`update` bindings |
| `native/luadap/CMakeLists.txt` | SHARED `luadap` → `bin/luadap.dll`; no embed |
| Delete from build | `gen_embed.py`, `embed_lua.cmake` |
| `script/test/run_debugee*.lua` | Switch to `require("luadap")` |
| Spec status | Mark cpp design **已实现** when green |

**Internal C API (locked names for cross-task consistency):**

```c
/* dap_framing.h */
typedef struct dap_recv_buf {
    char *data;
    size_t len;
    size_t cap;
} dap_recv_buf;

void dap_recv_buf_init(dap_recv_buf *b);
void dap_recv_buf_free(dap_recv_buf *b);
int dap_recv_buf_append(dap_recv_buf *b, const void *p, size_t n); /* 0 ok, -1 OOM */
/* Returns 1 if a full frame JSON body was extracted into *out_json (caller frees),
   0 if incomplete, -1 on fatal framing error. */
int dap_try_parse_frame(dap_recv_buf *b, char **out_json, size_t *out_len);

/* dap_json.h — thin helpers; session owns cJSON trees */
cJSON *dap_json_parse(const char *s, size_t n);
char *dap_json_print_unformatted(const cJSON *root); /* malloc'd; caller free */

/* dap_session.h */
typedef struct dap_session dap_session;

dap_session *dap_session_get(void); /* single global */
int dap_session_start(lua_State *L, const char *host, int port, int wait);
int dap_session_update(lua_State *L);
void dap_session_shutdown(lua_State *L, cJSON *disconnect_req /* nullable */);

/* lua_debug.h */
void lua_debug_install_hook(lua_State *L);
void lua_debug_clear_hook(lua_State *L);
```

---

### Task 1: Vendor cJSON + strip embed + skeleton `luadap`

**Files:**
- Create: `3rd/cJSON/cJSON.c`, `3rd/cJSON/cJSON.h` (upstream cJSON single-file pair)
- Create: `native/luadap/dap_session.h`, `dap_session.c` (minimal)
- Create: stub `dap_framing` / `dap_json` / `lua_debug` sources so the target links
- Modify: `native/luadap/CMakeLists.txt` — remove embed; list new sources; compile cJSON into luadap
- Modify: `native/luadap/luadap.c` — no embed / no `asyncsocket` preload; call `dap_session_*`
- Delete: `native/luadap/gen_embed.py`, `native/luadap/embed_lua.cmake`

**Interfaces:**
- Produces: `luaopen_luadap` exports `start`/`update`/`_VERSION`; `dap_session_start`/`update` callable
- Consumes: `as_net_init`, `as_socket_listen`, `as_socket_take_events`, `as_events_free`, `as_socket_stop`, `as_socket_destroy`

- [ ] **Step 1: Vendor cJSON**

Copy official `cJSON.c` / `cJSON.h` into `3rd/cJSON/`. Compile `cJSON.c` into the `luadap` target (no separate shared lib).

- [ ] **Step 2: Rewrite `native/luadap/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)

add_library(luadap SHARED
    luadap.c
    dap_framing.c
    dap_json.c
    dap_session.c
    lua_debug.c
    ${CMAKE_SOURCE_DIR}/3rd/cJSON/cJSON.c)

target_include_directories(luadap PRIVATE
    ${CMAKE_SOURCE_DIR}/3rd/lua-5.4.8/inc
    ${CMAKE_SOURCE_DIR}/3rd/cJSON
    ${CMAKE_SOURCE_DIR}/native/asyncsocket
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(luadap PRIVATE asyncsocket_static liblua)
if(WIN32)
    target_link_libraries(luadap PRIVATE ws2_32)
    target_compile_definitions(luadap PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0600
        _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(luadap PROPERTIES
    OUTPUT_NAME "luadap"
    PREFIX "")
```

- [ ] **Step 3: Minimal `dap_session` + empty companions**

`dap_session.c` for Task 1:

```c
#include "dap_session.h"
#include "poll_loop.h"
#include <string.h>
#include <stdio.h>
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

struct dap_session {
    as_socket *sock;
    int client_open;
    int configured;
    int dead;
    int close_pending;
    int hook_installed;
};

static dap_session g_sess;

dap_session *dap_session_get(void) { return &g_sess; }

int dap_session_start(lua_State *L, const char *host, int port, int wait) {
    (void)L;
    (void)wait;
    memset(&g_sess, 0, sizeof(g_sess));
    if (as_net_init() != 0) return -1;
    char err[256];
    g_sess.sock = as_socket_listen(host, port, err, sizeof(err));
    if (!g_sess.sock) {
        fprintf(stderr, "[luadap] listen failed: %s\n", err);
        return -1;
    }
    fprintf(stderr, "[luadap] listening on %s:%d\n", host, port);
    return 0;
}

int dap_session_update(lua_State *L) {
    (void)L;
    if (!g_sess.sock || g_sess.dead) return 0;
    size_t n = 0;
    as_event *evs = as_socket_take_events(g_sess.sock, &n);
    for (size_t i = 0; i < n; i++) {
        if (evs[i].type == AS_EVT_OPEN) g_sess.client_open = 1;
        if (evs[i].type == AS_EVT_CLOSE) g_sess.close_pending = 1;
    }
    as_events_free(evs, n);
    return 0;
}

void dap_session_shutdown(lua_State *L, cJSON *disconnect_req) {
    (void)L;
    (void)disconnect_req;
    if (g_sess.dead) return;
    g_sess.dead = 1;
    if (g_sess.sock) {
        as_socket_stop(g_sess.sock);
        as_socket_destroy(g_sess.sock);
        g_sess.sock = NULL;
    }
}
```

Provide empty `dap_framing.c` / `dap_json.c` / `lua_debug.c` (or minimal no-op functions declared in headers) so the library links.

- [ ] **Step 4: Rewrite `luadap.c` facade**

```c
#include <lua.h>
#include <lauxlib.h>
#include "dap_session.h"

#ifdef _WIN32
#define LUADAP_API __declspec(dllexport)
#else
#define LUADAP_API __attribute__((visibility("default")))
#endif

#define LUADAP_VERSION "0.2.0"

static int l_start(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int wait = 1;
    if (!lua_isnoneornil(L, 3)) wait = lua_toboolean(L, 3);
    if (dap_session_start(L, host, port, wait) != 0)
        return luaL_error(L, "luadap.start failed");
    return 0;
}

static int l_update(lua_State *L) {
    if (dap_session_update(L) != 0)
        return luaL_error(L, "luadap.update failed");
    return 0;
}

LUADAP_API int luaopen_luadap(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_start);
    lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_update);
    lua_setfield(L, -2, "update");
    lua_pushstring(L, LUADAP_VERSION);
    lua_setfield(L, -2, "_VERSION");
    return 1;
}
```

- [ ] **Step 5: Build and smoke `require`**

```powershell
cmake --build build --target luadap
# lua -e "package.cpath='bin/?.dll'; local d=require('luadap'); print(d._VERSION)"
```

Expected: prints `0.2.0`; no embedded debugger load.

- [ ] **Step 6: Commit**

```bash
git add 3rd/cJSON native/luadap
git rm -f native/luadap/gen_embed.py native/luadap/embed_lua.cmake
git commit -m "build: scaffold C++ luadap without Lua embed"
```

---

### Task 2: Framing + JSON + DAP handshake (wait + nowait)

**Files:**
- Implement: `dap_framing.c`, `dap_json.c`, expand `dap_session.c`
- Test: `script/test/test_dap_luadap_handshake.py`, `test_dap_luadap_nowait.py`

**Interfaces:**
- Consumes: Task 1 session + `as_socket_send`
- Produces: initialize → configurationDone; `configured=1`; wait loop; MESSAGE → recv_buf → parse → dispatch

- [ ] **Step 1: Implement framing**

Port `try_parse_one_dap_frame` from `debugger.lua` (lines ~79–94): find `\r\n\r\n`, parse `Content-Length` (case-insensitive), extract body, shift buffer. Incomplete → `0`; missing length → `-1`; success → `1` and malloc'd JSON body for caller to free.

- [ ] **Step 2: Implement send helpers**

`dap_session_send_raw`: increment `seq`, set `obj.seq`, `cJSON_PrintUnformatted`, build `Content-Length: N\r\n\r\n` + body, `as_socket_send`.  
`send_response(req, body, success, message)` and `send_event(event, body)` match Lua field names.

Ownership: caller creates `cJSON` trees; send helpers print and free the printed string; document whether they free the root (recommend: helpers do **not** free the request; they free response/event roots they build).

- [ ] **Step 3: Dispatch handshake commands**

Match `debugger.lua` handlers:

| command | behavior |
|---------|----------|
| `initialize` | capabilities + `initialized` event |
| `attach` | empty success body |
| `threads` | `{ threads = { { id=1, name="main" } } }` |
| `setExceptionBreakpoints` | empty success |
| `setBreakpoints` | normalize path; store line→condition; echo verified BPs |
| `configurationDone` | set `configured=1`; **do not** install hook here |

Unknown commands: respond `success=false` with a short message (do not crash).

Path normalize (same as Lua): strip leading `@`, `\`→`/`, lowercase drive letter, strip trailing `/`.

- [ ] **Step 4: Wire `update` pipeline**

```
take_events →
  OPEN: client_open=1
  MESSAGE: dap_recv_buf_append
  CLOSE: close_pending=1
loop up to 32x: try_parse_frame → dap_json_parse → dispatch by command
if close_pending: dap_session_shutdown (terminated details hardened in Task 7)
elif configured && !hook_installed && client_open && !dead:
  lua_debug_install_hook(L)  // no-op stub until Task 3
```

- [ ] **Step 5: Real `start` wait loop**

```c
if (wait) {
    while (!g_sess.configured && !g_sess.dead) {
        if (dap_session_update(L) != 0) {
            dap_session_shutdown(L, NULL);
            break;
        }
        short_sleep();
    }
}
```

- [ ] **Step 6: Run handshake tests**

```powershell
cmake --build build --target luadap
python script/test/test_dap_luadap_handshake.py
python script/test/test_dap_luadap_nowait.py
```

Expected: both exit 0.

- [ ] **Step 7: Commit**

```bash
git commit -am "feat: C++ DAP framing and handshake in luadap"
```

---

### Task 3: Line hook, pause_loop, breakpoint stop + continue

**Files:**
- Implement: `lua_debug.c`
- Modify: `script/test/run_debugee.lua` → `require("luadap")`
- Test: `script/test/test_dap_breakpoint.py` (must get `stopped`; vars may fail until Task 4)

**Interfaces:**
- Produces: host-thread `lua_sethook`; BP hit → `stopped` + pause until continue/disconnect
- Consumes: breakpoint map; `dap_session_update` inside pause_loop

- [ ] **Step 1: Switch `run_debugee.lua` to luadap**

Keep `work()` and the `local sum = x + y` line. Use `package.cpath = root .. "/bin/?.dll"` and `require("luadap")` + `start(..., true)`. Clear or avoid requiring `lua-runtime.debugger`.

- [ ] **Step 2: BP hit without conditions**

Port `on_line` BP branch: find first `@` user frame; lookup breakpoints[path][line]; empty/missing condition → hit.

- [ ] **Step 3: `pause_loop` + `continue`**

On hit: reset var maps (`next_ref=1000`), send `stopped`, loop `dap_session_update` + sleep while `paused`.  
`continue`: clear step, `paused=0`, response includes `allThreadsContinued=true`.

- [ ] **Step 4: Install hook only on host thread**

```c
lua_sethook(L, on_line_hook, LUA_MASKLINE, 0);
```

From `update` when `configured && !hook_installed && client_open && !dead`, and after successful wait-loop if still open. **Never** from `configurationDone` handler alone.

- [ ] **Step 5: Verify `stopped`**

```powershell
python script/test/test_dap_breakpoint.py
```

Expected: receive `stopped` with `reason=breakpoint`. May still fail on stackTrace/variables (Task 4).

- [ ] **Step 6: Commit**

```bash
git commit -am "feat: C++ line hook and breakpoint pause"
```

---

### Task 4: stackTrace, scopes, variables (incl. table members)

**Files:**
- Expand: `lua_debug.c` + session dispatch for `stackTrace` / `scopes` / `variables`
- Test: `script/test/test_dap_breakpoint.py` (full pass)

**Interfaces:**
- Ref scheme: locals `100000+frameId`, upvalues `200000+frameId`, tables from `1000` with per-stop reuse

- [ ] **Step 1: Walk user frames**

No reader coroutine: `lua_getstack` / `lua_getinfo` run on the paused thread inside `pause_loop` → `update` → handler. Keep `@` sources only. `frameId` 0 = closest to pause. Store Lua stack levels needed for `lua_getlocal`.

- [ ] **Step 2: scopes + variables**

Locals via `lua_getlocal` (skip names starting with `(`).  
Upvalues from frame function.  
Tables: alloc ref; circular ancestors → `value="table (circular)"`, `variablesReference=0`.

- [ ] **Step 3: Full breakpoint test**

```powershell
python script/test/test_dap_breakpoint.py
```

Expected: exit 0.

- [ ] **Step 4: Commit**

```bash
git commit -am "feat: C++ DAP stack frames and variables"
```

---

### Task 5: Stepping (next / stepIn / stepOut)

**Files:**
- Expand: step state in `lua_debug` / session handlers
- Modify: `script/test/run_debugee_step.lua` → luadap
- Test: `script/test/test_dap_step.py`

- [ ] **Step 1: Port step depth rules**

`current_depth()` counts `@` user frames.  
Handlers set `step` to `over` / `in` / `out` and `step_depth`.  
`on_line` applies the same rules as `debugger.lua` after the BP check.

- [ ] **Step 2: Run step test**

```powershell
python script/test/test_dap_step.py
```

Expected: exit 0.

- [ ] **Step 3: Commit**

```bash
git commit -am "feat: C++ DAP step in/over/out"
```

---

### Task 6: Conditional breakpoints + table cycle

**Files:**
- Expand: condition eval
- Modify: `run_debugee_cond.lua`, `run_debugee_cycle.lua` → luadap
- Test: `test_dap_condition.py`, `test_dap_table_cycle.py`

- [ ] **Step 1: Condition eval**

Build env from locals + upvalues; `luaL_loadstring` of `return (condition)`; `pcall`; failure → no hit (match Lua).

- [ ] **Step 2: Run both tests**

```powershell
python script/test/test_dap_condition.py
python script/test/test_dap_table_cycle.py
```

Expected: exit 0.

- [ ] **Step 3: Commit**

```bash
git commit -am "feat: C++ conditional breakpoints and table cycles"
```

---

### Task 7: Disconnect / partial frame + migrate remaining tests + docs

**Files:**
- Harden: `dap_session_shutdown` / `close_pending` drain order
- Migrate: inline/debugee scripts in `test_dap_handshake.py`, `test_dap_partial_frame.py`, `test_dap_disconnect.py`
- Update: `README.md`; mark `docs/superpowers/specs/2026-08-15-luadap-cpp-dap-design.md` status **已实现**

- [ ] **Step 1: Teardown parity**

Idempotent shutdown: `dead=1`, clear hook, optional disconnect response, `terminated` if client was open, stop/destroy socket, clear BP/refs.  
If CLOSE shares a batch with MESSAGE: drain frames before teardown.

- [ ] **Step 2: Migrate remaining DAP debugees to luadap**

Every regression entry uses `require("luadap")` + `bin/?.dll` (no `lua-runtime.debugger`).

- [ ] **Step 3: Full regression**

```powershell
python script/test/test_asyncsocket_smoke.py
python script/test/test_dap_luadap_handshake.py
python script/test/test_dap_luadap_nowait.py
python script/test/test_dap_handshake.py
python script/test/test_dap_breakpoint.py
python script/test/test_dap_step.py
python script/test/test_dap_disconnect.py
python script/test/test_dap_partial_frame.py
python script/test/test_dap_condition.py
python script/test/test_dap_table_cycle.py
```

Expected: all exit 0.

- [ ] **Step 4: Docs**

README: ship `luadap.dll`; DAP is native C++; `debugger.lua` is reference only. Spec → **已实现**.

- [ ] **Step 5: Commit**

```bash
git commit -am "test: migrate DAP suite to C++ luadap; mark cpp design done"
```

---

## Self-Review (plan vs spec)

| Spec requirement | Task |
|------------------|------|
| `start`/`update` only, no embed | 1 |
| cJSON + framing + session | 2 |
| wait / nowait | 2 |
| asyncsocket C API static | 1–2 |
| Hook on host thread | 3 |
| BP + continue | 3 |
| stack/scopes/variables + cycle | 4, 6 |
| step | 5 |
| condition BP | 6 |
| disconnect / partial / terminated | 7 |
| Migrate tests + mark 已实现 | 7 |

No TBD placeholders. Cross-task names (`dap_session_start`, `dap_try_parse_frame`, ref scheme) are consistent.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-15-luadap-cpp-dap.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks  
2. **Inline Execution** — this session with executing-plans and checkpoints  

Which approach?
