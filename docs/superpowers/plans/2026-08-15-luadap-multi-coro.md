# luadap Multi-Coroutine DAP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Map Lua coroutines to DAP threads so VS Code can list them, stop on breakpoints inside coroutines with the correct `threadId`, and inspect/step that coroutine — while keeping single-thread regressions green.

**Architecture:** New `coro_registry` tracks `threadId` ↔ `lua_State*` (main=`1`). On `start`, register main and wrap `coroutine.create`/`wrap`. Each tracked thread gets `lua_sethook`. Pause records `paused_id`/`paused_L`; `stopped` uses real `threadId` and `allThreadsStopped=false`. `stackTrace` walks only the paused thread; other known ids return empty frames. Step mode is bound to `paused_L`.

**Tech Stack:** Existing C luadap (`dap_session`, `lua_debug`), Lua 5.4 C API, Python DAP tests, CMake

**Spec:** `docs/superpowers/specs/2026-08-15-luadap-multi-coro-design.md`

## Global Constraints

- Public API: `start` / `update` unchanged; add `track(co, name?)`
- Main thread: `threadId=1`, `name="main"`
- `stopped.allThreadsStopped` = **false**; `stopped.threadId` = hitting coroutine
- Hook `coroutine.create` / `coroutine.wrap` on `start`; optional `luadap.track`
- Non-paused registered thread `stackTrace` → empty `stackFrames`; unknown id → failure
- `continue`: succeed only if `threadId` omitted **or** equals paused id (spec recommendation, locked here)
- Step applies only on the `lua_State*` that was paused when step was requested
- No DAP `pause` request
- English concise commits
- Build: `cmake --build build/msvc --target luadap` (or project’s existing build dir)

---

## File Structure

| File | Responsibility |
|------|----------------|
| `native/luadap/coro_registry.h` | Registry API |
| `native/luadap/coro_registry.c` | id map, track, wrap install/uninstall, purge dead, list for `threads` |
| `native/luadap/lua_debug.c/.h` | Hook any `L`; pause sets paused id; step checks `L` |
| `native/luadap/dap_session.c/.h` | `threads`, `send_stopped`, `stackTrace` by id, continue rule, start/shutdown hooks into registry |
| `native/luadap/luadap.c` | Export `track` |
| `native/luadap/CMakeLists.txt` | Add `coro_registry.c` |
| `script/test/run_debugee_coro.lua` | Multi-coro debugee |
| `script/test/test_dap_coro.py` | New acceptance test |
| Spec / README | Mark 已实现; short note on multi-coro |

**Locked C API:**

```c
/* coro_registry.h */
void coro_registry_clear(lua_State *mainL); /* shutdown: unhook all, unwrap, free */
int coro_registry_track(lua_State *mainL, lua_State *co, const char *name_opt);
/* returns threadId (>=1), or 0 on error. Idempotent if already tracked. */
int coro_registry_id_for(lua_State *co);          /* 0 if unknown */
lua_State *coro_registry_state_for(int thread_id); /* NULL if unknown/dead purged */
void coro_registry_purge_dead(lua_State *mainL);
/* Fill DAP threads array (cJSON array*). Purges dead first. */
int coro_registry_append_threads_json(cJSON *threads_array);
void coro_registry_install_wrappers(lua_State *mainL); /* wrap create/wrap */
void coro_registry_uninstall_wrappers(lua_State *mainL);

/* dap_session additions */
int dap_session_paused_thread_id(void);
lua_State *dap_session_paused_L(void);
void dap_session_set_paused_thread(lua_State *L, int thread_id);
int dap_session_send_stopped(const char *reason); /* uses paused thread id; allThreadsStopped=false */
```

---

### Task 1: Coroutine registry + wrap + `track` + `threads` list

**Files:**
- Create: `native/luadap/coro_registry.h`, `coro_registry.c`
- Modify: `native/luadap/CMakeLists.txt`, `dap_session.c` (`start`/`shutdown`/`handle_threads`/`update` purge), `luadap.c`
- Test: `script/test/test_dap_coro_threads.py` + small debugee (or first half of coro test)

**Interfaces:**
- Produces: `coro_registry_*` as above; `luadap.track`; `threads` returns main + live coros
- Consumes: `lua_debug_install_hook` / `clear_hook` (call from `track` when session already has hooks installed — if `hook_installed` flag set; otherwise `start` wait path installs on main only until Task 2 unifies)

- [ ] **Step 1: Write failing threads smoke test**

Create `script/test/run_debugee_coro_threads.lua`:

```lua
local root = arg[1] or "."
local port = tonumber(arg[2] or 18200)
root = root:gsub("\\", "/")
package.path = ""
package.cpath = root .. "/bin/?.dll"
local dap = require("luadap")
dap.start("127.0.0.1", port, true)
local co = coroutine.create(function()
  local x = 1
  while true do coroutine.yield(x) end
end)
assert(dap.track) -- will fail until exported
print("CORO_READY")
io.stdout:flush()
-- keep process alive for client
while true do
  dap.update()
  -- busy wait lightly
end
```

Create `script/test/test_dap_coro_threads.py` that: handshake, `configurationDone`, create is on debugee side before ready — simpler: debugee creates coro **after** listen via wrapping so after handshake client sends `threads` and expects `len >= 2` with id 1 named main.

Better debugee for Step 1:

```lua
-- after start(wait=true), wrappers already active:
local co = coroutine.create(function() return 1 end)
print("CORO_READY")
io.stdout:flush()
while true do dap.update() end
```

Test asserts `threads` response has ids including `1` and another id, names contain `main`.

- [ ] **Step 2: Run test — expect fail**

```powershell
cmake --build build/msvc --target luadap
python script/test/test_dap_coro_threads.py
```

Expected: fail (no `track` and/or only one thread).

- [ ] **Step 3: Implement `coro_registry`**

Store entries: `{ int id; int reg_ref; char name[64]; }`. Main always id 1.

`coro_registry_track`:
1. If `co` already tracked → return existing id (optional rename if `name_opt`).
2. Else allocate next id (start at 2 after main registered).
3. `lua_pushthread(co)` from main via `lua_xmove` or push thread object on main and `luaL_ref(main, LUA_REGISTRYINDEX)`.
4. Name default `coro-%d`.
5. If debugger hooks are active (`dap_session` exposes `dap_session_hooks_active()` or check `hook_installed`), call `lua_debug_install_hook(co)`.

`install_wrappers`: replace `coroutine.create` / `coroutine.wrap` with C closures that call originals then `coro_registry_track`. Save originals in registry under lightuserdata keys.

`purge_dead`: for each non-main entry, `lua_rawgeti` + `lua_status` / `lua_isthread`; if dead or nil, clear hook, unref, remove.

- [ ] **Step 4: Wire session + `luadap.track`**

In `dap_session_start` after listen setup:
```c
coro_registry_clear(L);
coro_registry_track(L, L, "main"); /* forces id 1 */
coro_registry_install_wrappers(L);
```

In `handle_threads`: purge then `coro_registry_append_threads_json`.

In `dap_session_shutdown`: `coro_registry_uninstall_wrappers` + `coro_registry_clear`.

In `dap_session_update`: periodic `coro_registry_purge_dead(L)`.

`luadap.c`:
```c
static int l_track(lua_State *L) {
    lua_State *co = lua_tothread(L, 1);
    const char *name = NULL;
    int id;
    if (!co) return luaL_error(L, "track: expected thread");
    if (!lua_isnoneornil(L, 2)) name = luaL_checkstring(L, 2);
    id = coro_registry_track(L, co, name);
    if (id == 0) return luaL_error(L, "track failed");
    lua_pushinteger(L, id);
    return 1;
}
```

Ensure main registration assigns **exactly** id 1 (special-case first track of mainL).

- [ ] **Step 5: Re-run threads test — expect pass**

- [ ] **Step 6: Commit**

```bash
git commit -am "feat: coroutine registry, wrap create/wrap, luadap.track"
```

---

### Task 2: Per-coro hooks + `stopped` with real `threadId`

**Files:**
- Modify: `lua_debug.c` (`pause_loop`), `dap_session.c` (`send_stopped`, paused fields), `coro_registry.c` (always install hook when session hooks active)
- Test: `script/test/run_debugee_coro.lua`, `script/test/test_dap_coro.py`

**Interfaces:**
- Produces: `dap_session_set_paused_thread` / `paused_thread_id` / `paused_L`; `send_stopped` uses them + `allThreadsStopped=false`
- Consumes: `coro_registry_id_for(L)`

- [ ] **Step 1: Write failing multi-coro breakpoint test**

`run_debugee_coro.lua`:

```lua
local root = arg[1] or "."
local port = tonumber(arg[2] or 18201)
root = root:gsub("\\", "/")
package.path = ""
package.cpath = root .. "/bin/?.dll"
local dap = require("luadap")
dap.start("127.0.0.1", port, true)

local function worker()
  local a = 10
  local b = 20
  local sum = a + b  -- breakpoint target
  return sum
end

local co = coroutine.create(worker)
coroutine.resume(co)
print("DEBUGEE_DONE")
io.stdout:flush()
```

`test_dap_coro.py`: find line with `local sum = a + b`, setBreakpoints on that file, configurationDone, wait `stopped`, assert:
- `body["threadId"] != 1` (or `>= 2`)
- `body.get("allThreadsStopped") is False`
- `threads` lists both main and that id

- [ ] **Step 2: Run — expect fail** (threadId still 1 or no stop in coro)

- [ ] **Step 3: Install hook on every track; pause records id**

When main gets hook in `update`/wait, also iterate registry and `lua_debug_install_hook` each co (add `coro_registry_install_hooks_all`).

`pause_loop`:
```c
int tid = coro_registry_id_for(L);
if (tid == 0) tid = 1;
dap_session_set_paused_thread(L, tid);
dap_session_set_paused(1);
...
dap_session_send_stopped(reason);
```

`dap_session_send_stopped`:
```c
cJSON_AddNumberToObject(body, "threadId", dap_session_paused_thread_id());
cJSON_AddBoolToObject(body, "allThreadsStopped", 0);
```

Ensure `coroutine.create` wrapper tracks **before** the coro runs user code (track right after create returns) so resume hits hooked state.

- [ ] **Step 4: Pass `test_dap_coro.py` for stopped assertions** (stack/vars may still use wrong L — next task if needed; if pause_loop already passes coro `L` into `update`, stack may already work)

- [ ] **Step 5: Commit**

```bash
git commit -am "feat: DAP stopped threadId for Lua coroutines"
```

---

### Task 3: `stackTrace` by `threadId` + locals on paused coro + `continue` rule

**Files:**
- Modify: `dap_session.c` `handle_stack_trace`, `handle_continue`, `handle_next`/`step*` (use `paused_L` for depth)
- Test: extend `test_dap_coro.py`

**Interfaces:**
- `handle_stack_trace`: read `arguments.threadId`; if unknown → `success=false`; if `!= paused_id` → empty frames; if `== paused_id` → `lua_debug_stack_frames(paused_L)`
- `handle_variables` / scopes: use `dap_session_paused_L()` when paused (not the `L` from update if they diverge)
- `continue`: if args.threadId present and `!= paused_id` → fail; else clear pause

- [ ] **Step 1: Extend test**

After stopped:
```python
tid = stopped["body"]["threadId"]
c.send_request("stackTrace", {"threadId": tid})
# expect frames non-empty, source path matches debugee
c.send_request("stackTrace", {"threadId": 1})
# expect stackFrames == []
# scopes/variables on tid frame: assert local a or b exists
c.send_request("continue", {"threadId": tid})
```

- [ ] **Step 2: Fail then implement routing**

```c
static void handle_stack_trace(lua_State *L, cJSON *req) {
    cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
    cJSON *tidj = args ? cJSON_GetObjectItemCaseSensitive(args, "threadId") : NULL;
    int tid = (tidj && cJSON_IsNumber(tidj)) ? (int)tidj->valuedouble : 1;
    lua_State *target = coro_registry_state_for(tid);
    cJSON *body;
    if (!target) {
        send_response(req, NULL, 0, "unknown thread");
        return;
    }
    if (!dap_session_is_paused() || tid != dap_session_paused_thread_id()) {
        body = cJSON_CreateObject();
        cJSON_AddArrayToObject(body, "stackFrames");
        cJSON_AddNumberToObject(body, "totalFrames", 0);
        send_response(req, body, 1, NULL);
        return;
    }
    send_response(req, lua_debug_stack_frames(dap_session_paused_L()), 1, NULL);
}
```

`handle_continue`:
```c
/* parse threadId if present; if present && != paused_id → success=false */
```

Step handlers: compute depth with `dap_session_paused_L()` (or `L` if equal).

Store `step_L` when entering step; in `on_line_hook`, only honor step modes if `L == step_L` (set in Task 4 if not done here — **do it in this task** to avoid flaky step across coros).

```c
/* in handle_next etc */
g_sess.step_L = dap_session_paused_L();
/* in on_line step branch */
if (mode != NONE && L != dap_session_step_L()) return; /* after BP check */
```

- [ ] **Step 3: Pass extended `test_dap_coro.py`**

- [ ] **Step 4: Commit**

```bash
git commit -am "feat: stackTrace/continue scoped to paused coroutine"
```

---

### Task 4: Regression + docs + mark spec implemented

**Files:**
- Modify: `docs/superpowers/specs/2026-08-15-luadap-multi-coro-design.md` status → **已实现**
- Modify: `README.md` — note multi-coro / `track`
- Verify: full DAP suite

- [ ] **Step 1: Run full regression**

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
python script/test/test_dap_coro_threads.py
python script/test/test_dap_coro.py
```

Expected: all exit 0. Fix any single-thread breakage (main still id 1; `allThreadsStopped=false` must not break clients that ignore the field).

- [ ] **Step 2: Docs**

Spec status **已实现**. README: coroutines via wrapped `coroutine.create`/`wrap`; optional `dap.track(co, name)`.

- [ ] **Step 3: Commit**

```bash
git commit -am "test: multi-coro DAP regression; mark multi-coro design done"
```

---

## Self-Review (plan vs spec)

| Spec item | Task |
|-----------|------|
| Registry + main id 1 | 1 |
| wrap create/wrap + track | 1 |
| threads list | 1 |
| Per-coro hook + stopped threadId + allThreadsStopped=false | 2 |
| stackTrace empty vs full | 3 |
| continue threadId rule | 3 |
| step bound to paused L | 3 |
| Regression + 已实现 | 4 |

No TBD. `continue` rule locked: omit or match paused id.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-15-luadap-multi-coro.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks  
2. **Inline Execution** — this session with executing-plans and checkpoints  

Which approach?
