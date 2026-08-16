# Multi lua_State + Cross-Thread DAP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let multiple `lua_State`s in one process share one DAP listen/session (join via same host/port `dap.start`), show flat Threads with state-prefixed names, pause only the hitting flow while other OS threads keep running, and eliminate data races via session mutex + lock-free sleep in `pause_loop`.

**Architecture:** Process-wide non-recursive `session_mutex` guards session/registry/asyncsocket pump and DAP send. Paused set is multi-entry (`threadId → {L, cond}`). Inbound DAP messages queue; Lua-touching commands run only on the OS thread that owns the paused `L`. `state_registry` + extended `coro_registry` support multi-mainL join. No dedicated DAP pump thread.

**Tech Stack:** C11 luadap, Win32 `CRITICAL_SECTION` + `CONDITION_VARIABLE` (POSIX `pthread_mutex`/`pthread_cond` behind thin `dap_sync.h`), existing Python `DapClient`, new C test host under `test/`, CMake

**Spec:** `docs/superpowers/specs/2026-08-16-multi-lua-state-design.md`

## Global Constraints

- One DAP listen / one DAP client; second TCP accept still closed
- `allThreadsStopped` always **false**
- Same `(host,port)` `start` joins; different port → error
- Optional `dap.start(host, port, wait [, name])`
- Single state + no name → main thread name `main`; multi-state no name → `state-N`
- Hold `session_mutex` never call into user Lua / `lua_pcall` / stack walk
- One `lua_State*` owned by one OS thread (Lua rule unchanged)
- `continue`/`step*`: require `threadId` matching a paused entry; if omitted and exactly one paused → that one; else fail
- English concise commits
- Build: `cmake -S . -B build "-DLUA_VERSION=5.4"` then `cmake --build build --config Release --target luadap`
- Tests live under `test/` (not `script/test/`)

---

## File Structure

| File | Responsibility |
|------|----------------|
| `native/luadap/dap_sync.h` / `dap_sync.c` | `dap_mutex_*`, `dap_cond_*` (Win32 / pthread) |
| `native/luadap/state_registry.h` / `.c` | mainL list, names, join lookup |
| `native/luadap/coro_registry.c/.h` | multi-mainL entries, prefixed names, lock assumed held by caller or internal lock policy (document: **all coro_registry APIs require caller to hold session_mutex** OR take mutex internally — **lock: call under session_mutex**) |
| `native/luadap/dap_session.c/.h` | mutex, paused set, request queue, start join, dispatch split |
| `native/luadap/lua_debug.c` | hook → paused set; pause_loop unlock-wait-pump |
| `native/luadap/luadap.c` | optional 4th `name` arg to `start` |
| `native/luadap/CMakeLists.txt` | add new `.c` files |
| `test/multi_state_dap_host.c` | dual-state host (single-thread + optional dual-thread mode) |
| `test/CMakeLists.txt` + root `CMakeLists.txt` | build host → `bin/` |
| `test/test_dap_multi_state.py` | join, names, port mismatch, same-thread pause |
| `test/test_dap_multi_state_mt.py` | two OS threads, heartbeat, dual stopped, continue by id |
| `README.md` | multi-state + MT notes |

**Locked sync API:**

```c
/* dap_sync.h */
void dap_mutex_init(void);
void dap_mutex_destroy(void);
void dap_mutex_lock(void);
void dap_mutex_unlock(void);
typedef struct dap_cond dap_cond;
void dap_cond_init(dap_cond *c);
void dap_cond_destroy(dap_cond *c);
void dap_cond_wait(dap_cond *c);   /* must hold mutex; atomically unlock+wait; re-acquire */
void dap_cond_signal(dap_cond *c);
```

**Locked paused / queue shapes (in dap_session.c):**

```c
enum { DAP_PAUSED_MAX = 64, DAP_REQ_Q_MAX = 64 };

typedef struct {
  int thread_id;
  lua_State *L;
  dap_cond cond;
  int step_mode;   /* DAP_STEP_* */
  int step_depth;
} dap_paused_entry;

/* Lua-touching commands deferred until owner pause_loop/update */
typedef struct {
  cJSON *msg; /* owned */
} dap_queued_req;
```

---

### Task 1: Session mutex + wrap existing start/update/send

**Files:**
- Create: `native/luadap/dap_sync.h`, `native/luadap/dap_sync.c`
- Modify: `native/luadap/CMakeLists.txt`, `native/luadap/dap_session.c`, `native/luadap/luadap.c` (if start path needs lock)
- Test: existing `test/test_dap_luadap_handshake.py` (regression)

**Interfaces:**
- Produces: `dap_mutex_lock/unlock`, init on first `dap_session_start`, destroy on process shutdown path if any
- Consumes: none

- [ ] **Step 1: Add `dap_sync` for Win32**

```c
/* dap_sync.c — WIN32 */
#include "dap_sync.h"
#include <windows.h>
static CRITICAL_SECTION g_cs;
static int g_inited;

void dap_mutex_init(void) {
  if (g_inited) return;
  InitializeCriticalSection(&g_cs);
  g_inited = 1;
}
void dap_mutex_lock(void) { EnterCriticalSection(&g_cs); }
void dap_mutex_unlock(void) { LeaveCriticalSection(&g_cs); }
/* dap_cond: CONDITION_VARIABLE + SleepConditionVariableCS */
```

Add `#else` pthread stubs for non-Windows so the file compiles on CI matrix Linux if ever used; Windows is primary.

- [ ] **Step 2: Lock `dap_session_update` and `dap_session_start` bodies**

At top of `dap_session_update` / `dap_session_start`: `dap_mutex_init(); dap_mutex_lock();`  
`goto out` / every return path: `dap_mutex_unlock();`  
Also lock around `send_response` / `send_event` call sites used from update/dispatch **or** lock the whole `dispatch` when called from update (current single-threaded path).

**Do not yet change pause_loop** (still single paused; still may call update while conceptually “paused” on same thread — update already re-enters: **use a reentrancy depth or keep CRITICAL_SECTION which is recursive by nature!**).

**Lock decision override:** Win32 `CRITICAL_SECTION` **is recursive**. Spec preferred non-recursive; for Task 1 use CRITICAL_SECTION (recursive) to avoid immediate deadlock when `pause_loop` → `update` → lock. Document in `dap_sync.h`: “recursive on Win32; pause_loop may call update.”

- [ ] **Step 3: Build + regression**

```powershell
cmake --build build --config Release --target luadap
python test/test_dap_luadap_handshake.py
python test/test_dap_coro.py
```

Expected: both `ok`

- [ ] **Step 4: Commit**

```bash
git add native/luadap/dap_sync.h native/luadap/dap_sync.c native/luadap/CMakeLists.txt native/luadap/dap_session.c
git commit -m "feat(luadap): session mutex around DAP start/update"
```

---

### Task 2: Multi-paused set + threadId continue/step + unlock wait

**Files:**
- Modify: `native/luadap/dap_session.c`, `native/luadap/dap_session.h`, `native/luadap/lua_debug.c`
- Test: `test/test_dap_coro.py`, `test/test_dap_step.py` (regression); extend coro test continue with explicit threadId if not already

**Interfaces:**
- Produces:
  - `int dap_session_paused_contains(int thread_id);`
  - `int dap_session_pause_enter(lua_State *L, int thread_id, const char *reason);` /* lock, add, send stopped, unlock; then caller waits */
  - `void dap_session_pause_wait(int thread_id);` /* while contains: update(unlocked sections) + cond wait */
  - `int dap_session_resume_thread(int thread_id);` /* continue clears pause + signal */
- Consumes: Task 1 mutex/cond

- [ ] **Step 1: Replace single `paused_L` / `paused_thread_id` with array**

```c
static dap_paused_entry g_paused[DAP_PAUSED_MAX];
static int g_paused_n;

static dap_paused_entry *paused_find(int tid);
static int paused_add(lua_State *L, int tid);
static void paused_remove(int tid); /* signal cond */
```

Map old APIs:
- `dap_session_is_paused()` → `g_paused_n > 0` **or** “is **this** L paused?” — hooks need `dap_session_is_L_paused(L)`.
- `dap_session_paused_L()` → **deprecated for multi**; prefer `dap_session_paused_L_for(tid)`.
- Keep `dap_session_paused_L()` as “first paused” only for transitional compile; remove by end of task.

- [ ] **Step 2: Rewrite `pause_loop` in `lua_debug.c`**

```c
static void pause_loop(lua_State *L, const char *reason) {
  int tid = coro_registry_id_for(L);
  if (tid <= 0) tid = 1;
  dap_session_pause_enter(L, tid, reason); /* sends stopped; adds to set */
  while (dap_session_paused_contains(tid)) {
    if (dap_session_update(L) != 0) { /* pumps; may resume via continue */
      dap_session_shutdown(L, NULL);
      break;
    }
    if (!dap_session_paused_contains(tid)) break;
    dap_session_pause_wait_idle(tid); /* cond_wait with mutex, or Sleep(1) under unlock */
  }
}
```

`dap_session_update` must **not** assume single paused when applying continue.

- [ ] **Step 3: `handle_continue` / step handlers**

```c
static int resolve_target_tid(cJSON *req, int *out_tid) {
  cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "arguments");
  cJSON *tj = args ? cJSON_GetObjectItemCaseSensitive(args, "threadId") : NULL;
  if (tj && cJSON_IsNumber(tj)) { *out_tid = (int)tj->valuedouble; return 0; }
  if (g_paused_n == 1) { *out_tid = g_paused[0].thread_id; return 0; }
  return -1;
}
```

On success: set step fields on that entry (for step*), `paused_remove(tid)`, `dap_cond_signal`.

- [ ] **Step 4: `stackTrace` only if tid paused**

If `paused_find(tid)`: walk that `L` (still same-thread in current tests).  
Else if known thread: empty frames.  
Else: failure.

- [ ] **Step 5: Regression**

```powershell
cmake --build build --config Release --target luadap
python test/test_dap_luadap_handshake.py
python test/test_dap_breakpoint.py
python test/test_dap_step.py
python test/test_dap_coro.py
python test/test_dap_evaluate.py
```

Expected: all ok

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(luadap): multi-paused set and per-threadId continue/step"
```

---

### Task 3: DAP request queue (Lua-touching commands owner-thread only)

**Files:**
- Modify: `native/luadap/dap_session.c`
- Test: regression suite; later MT test proves cross-thread inspect

**Interfaces:**
- Produces: inbound MESSAGE → parse → if command ∈ {`stackTrace`,`scopes`,`variables`,`evaluate`} enqueue; else `dispatch_session(msg)` immediately under lock (no Lua)
- `dap_session_update(L)` after pump: `drain_queue_for(L)` — dispatch Lua cmds whose `threadId` maps to paused entry with `entry->L` related to this `L` (same main state or exact co)

**Classify commands:**

| Immediate (any updater thread) | Owner pause thread only |
|-------------------------------|-------------------------|
| initialize, attach/launch, setBreakpoints, setExceptionBreakpoints, threads, continue, next, stepIn, stepOut, configurationDone, disconnect, terminate | stackTrace, scopes, variables, evaluate |

- [ ] **Step 1: Queue + drain**

```c
static dap_queued_req g_q[DAP_REQ_Q_MAX];
static int g_q_n;

static int cmd_needs_lua(const char *cmd);
static void enqueue_req(cJSON *msg); /* steals or copies */
static void drain_lua_reqs_for(lua_State *self_L);
```

When draining: unlock before `lua_debug_*`, re-lock after (or run Lua fully unlocked after snapshotting args).

- [ ] **Step 2: Wire MESSAGE handler to enqueue vs dispatch**

- [ ] **Step 3: Regression** (same commands as Task 2)

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(luadap): queue Lua DAP requests for owner paused thread"
```

---

### Task 4: `state_registry` + `start` join + optional name

**Files:**
- Create: `native/luadap/state_registry.h`, `native/luadap/state_registry.c`
- Modify: `native/luadap/dap_session.c` (`dap_session_start`), `native/luadap/luadap.c`, `CMakeLists.txt`
- Test: will fully pass in Task 6; interim: handshake still works with one state

**Interfaces:**

```c
int state_registry_count(void);
int state_registry_add(lua_State *mainL, const char *name_opt); /* id>=1 or 0 */
int state_registry_has(lua_State *mainL);
const char *state_registry_name(lua_State *mainL);
void state_registry_clear(void);
/* naming rule applied inside add */
```

- [ ] **Step 1: Implement registry**

On `add`: if exists return existing id.  
Name: if `name_opt` use it; else if count==0 before add → `"main"` for display of main thread (store state name `""` or `"main"`); if count>=1 before add → `snprintf name "state-%d"`.  
Spec: “仅一个 state 且未传 name 时名为 `main`；多 state 未传 name 时为 `state-N`” — when second state added, **rename first** if it was default `main`? **Locked: do not rename first**; first stays `main`, second `state-2`. Or first becomes `state-1` when second joins — **locked: first keeps `main` if it had no explicit name; subsequent defaults `state-2`, `state-3`…** (skip `state-1` to avoid confusion) → cleaner: **first default `state-1`, only if never multi…** Spec says single → `main`. Implement exactly: at add time if `state_registry_count()==0 && !name` → store `"main"`; else if !name → `state-{id}`.

- [ ] **Step 2: `dap_session_start` join logic**

```c
int dap_session_start(lua_State *L, const char *host, int port, int wait) {
  dap_mutex_lock();
  if (g_sess.listening) {
    if (strcmp(host, g_sess.host) != 0 || port != g_sess.port) {
      dap_mutex_unlock();
      return -1; /* luadap.c maps to luaL_error */
    }
    if (!state_registry_has(L)) {
      state_registry_add(L, name);
      coro_registry_track_main(L); /* Task 5 may flesh */
      if (g_sess.hook_installed) lua_debug_install_hook(L);
      coro_registry_install_wrappers(L);
    }
    /* wait if needed */
    dap_mutex_unlock();
    return 0;
  }
  /* first start: copy host/port, listen, add state, … existing path */
}
```

Pass `name` from Lua:

```c
static int l_start(lua_State *L) {
  const char *host = luaL_checkstring(L, 1);
  int port = (int)luaL_checkinteger(L, 2);
  int wait = lua_isnoneornil(L, 3) ? 1 : lua_toboolean(L, 3);
  const char *name = lua_isnoneornil(L, 4) ? NULL : luaL_checkstring(L, 4);
  if (dap_session_start_ex(L, host, port, wait, name) != 0)
    return luaL_error(L, "luadap.start failed");
  return 0;
}
```

- [ ] **Step 3: Build + single-state regression**

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(luadap): multi-state start join on same host/port"
```

---

### Task 5: `coro_registry` multi-mainL + prefixed thread names

**Files:**
- Modify: `native/luadap/coro_registry.c`, `native/luadap/coro_registry.h`
- Modify: `dap_session` `handle_threads`, start/join/shutdown to install wrappers per mainL
- Test: `test/test_dap_coro.py` must still see breakpoint in coro (names may gain prefix only when multi-state)

**Interfaces:**
- Change track to require `mainL` of owning state (already first arg)
- Remove single `g_mainL` exclusivity; allow multiple mains each with wrappers
- `append_threads_json`: name = state_name for main entry; `snprintf("%s/%s", state, coro)` for others
- Main thread id: **no longer force global id=1 only once** — first main gets an id (can keep first registered main as id=1 for compat); each additional main gets new id from `g_next_id`

**Compat lock:** First mainL registered still uses `threadId=1` named `main` when alone — existing tests that assume id=1 keep working.

- [ ] **Step 1: Per-main wrapper install** (registry key in that L’s registry)

- [ ] **Step 2: Prefixed names when `state_registry_count() > 1`**; if count==1 keep `main` / `coro-N` without prefix

- [ ] **Step 3: `reset_client` / shutdown iterate all states** (hooks flag or per-L clear on owning update)

- [ ] **Step 4: Regression including coro**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(luadap): coro registry supports multiple main states"
```

---

### Task 6: Test host + Python same-thread multi-state tests

**Files:**
- Create: `test/multi_state_dap_host.c`, `test/CMakeLists.txt`
- Modify: root `CMakeLists.txt` (`add_subdirectory(test)` when LUA 5.4 or always)
- Create: `test/test_dap_multi_state.py`
- Create: Lua snippets embedded or `test/run_ms_a.lua` / `run_ms_b.lua`

**Host behavior:**

```c
/* multi_state_dap_host.c */
/* args: --port N [--mt] */
/* Create L_a, L_b; set package.cpath to bin/?.dll; load strings that call dap.start */
/* Single-thread mode: loop { resume chunks / call update on both } */
/* Print "listening on host:port" once when first start returns */
```

Minimal scripts:

```lua
-- injected for A
package.cpath = [[BIN]] .. "/?.dll"
local dap = require("luadap")
dap.start("127.0.0.1", PORT, false, "logic")
function tick() local x=1; return x end  -- BP line known
```

- [ ] **Step 1: CMake executable `multi_state_dap_host` linking `luadap` (shared) + `liblua`**

- [ ] **Step 2: Write `test_dap_multi_state.py`**

```python
# start host, DAP initialize/attach, threads request
# assert any(t["name"]=="logic" for t in threads) and any(...=="ui")
# setBreakpoints on A file/line, configurationDone
# wait stopped threadId == logic's id
# stackTrace other main -> empty
# continue that threadId
```

Port mismatch subtest: host mode or small Lua — second start different port should fail (host prints FAIL_JOIN and exits 2); Python asserts.

- [ ] **Step 3: Run**

```powershell
cmake --build build --config Release --target multi_state_dap_host luadap
python test/test_dap_multi_state.py
```

Expected: pass

- [ ] **Step 4: Commit**

```bash
git commit -m "test: same-thread multi-state DAP host and Python coverage"
```

---

### Task 7: Cross-thread test + dual stopped

**Files:**
- Extend: `test/multi_state_dap_host.c` (`--mt` spawns two threads)
- Create: `test/test_dap_multi_state_mt.py`

**MT host:**

```c
static volatile long g_heart_b;
/* thread B: loop { run lua tick; dap.update; InterlockedIncrement(&g_heart_b); } */
/* thread A: loop until paused then pause_loop inside hook */
/* main: start both, print HEART periodically optional */
```

Python:

1. Attach, set BP on A and B  
2. On A stopped: sample `HEART` file or stdout counter twice with sleep — must increase  
3. Continue neither yet; wait for B stopped (second stopped event)  
4. `threads` shows both; continue A then B by id  

- [ ] **Step 1: Implement `--mt`**

- [ ] **Step 2: Python MT test**

- [ ] **Step 3: Run both multi-state tests + full smoke list from README**

- [ ] **Step 4: Commit**

```bash
git commit -m "test: cross-thread multi-state pause isolation"
```

---

### Task 8: README + mark spec implemented

**Files:**
- Modify: `README.md`, `docs/superpowers/specs/2026-08-16-multi-lua-state-design.md` (status → 已实现)

- [ ] **Step 1: README section** — multi `dap.start` join, optional name, MT rules (one L per OS thread; hold mutex never in user Lua; same-thread pause stalls siblings), link tests

- [ ] **Step 2: Spec status line**

- [ ] **Step 3: Commit**

```bash
git commit -m "docs: multi-state DAP usage and mark spec implemented"
```

---

## Spec coverage self-check

| Spec requirement | Task |
|------------------|------|
| Shared session join same host/port | 4 |
| Different port error | 4, 6 |
| Flat threads + name prefix | 5 |
| Pause only hitting flow | 2 |
| Other OS threads continue | 7 |
| Multi paused / allThreadsStopped false | 2, 7 |
| session_mutex, no Lua under lock | 1, 3 |
| pause_loop unlock wait | 2 |
| Lua DAP cmds owner thread | 3 |
| continue/step by threadId | 2 |
| Optional start name | 4 |
| Tests same-thread + MT | 6, 7 |
| README | 8 |
| Single-state compat (main id 1) | 5 |

## Placeholder scan

No TBD/TODO left in task steps; sync API and naming rules locked above.

---

## Execution handoff

Plan saved to `docs/superpowers/plans/2026-08-16-multi-lua-state.md`.
