# luadap DAP Evaluate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add DAP `evaluate` to C++ `luadap` with Watch/Hover (read-only) and REPL (statements + writeback to locals/upvalues/_G), and turn on `supportsEvaluateForHovers`.

**Architecture:** `handle_evaluate` in `dap_session` calls `lua_debug_evaluate` on the paused `lua_State*`. Build a per-frame env table (locals + upvalues, `__index=_G`). Watch/hover load `return (expr)`. REPL tries expression then statement; `__newindex` C closure writes via `lua_setlocal`/`lua_setupvalue` or `_G`. Results use existing `format_var` / table refs.

**Tech Stack:** Lua 5.4 C API, cJSON, existing `lua_debug` / `dap_session`, Python DAP tests

**Spec:** `docs/superpowers/specs/2026-08-15-luadap-evaluate-design.md`

## Global Constraints

- `supportsEvaluateForHovers = true`
- Contexts: `watch` | `hover` | `repl` (default `watch`)
- Only while paused; else `success=false`
- `frameId` default 0 on paused coroutine
- Watch/hover: read-only `return (expr)`
- REPL: expr or statements; writeback local/upvalue/_G
- Multi-return: first value only; no return → `result="nil"`
- No new public Lua module API
- English concise commits
- Build: `cmake --build build/msvc --target luadap`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `native/luadap/lua_debug.h` | Declare `lua_debug_evaluate` |
| `native/luadap/lua_debug.c` | Env build, evaluate, REPL writeback; optional share with BP cond |
| `native/luadap/dap_session.c` | Capability + `handle_evaluate` dispatch |
| `script/test/test_dap_evaluate.py` | Acceptance tests |
| `script/test/run_debugee.lua` | Reuse (has `x`,`y`,`player`) or tiny dedicated debugee |
| Spec | Mark **已实现** |

**Locked API:**

```c
/* context: 0=watch, 1=hover, 2=repl */
enum { LUA_EVAL_WATCH = 0, LUA_EVAL_HOVER = 1, LUA_EVAL_REPL = 2 };

/* On success: returns owned cJSON body {result,type,variablesReference}.
 * On failure: returns NULL and sets *err_msg to malloc'd string (caller frees)
 * or static string (document which — use malloc'd). */
cJSON *lua_debug_evaluate(lua_State *L, int frame_id, int context,
                          const char *expression, char **err_msg);
```

Caller must use paused L (`dap_session_paused_L()`).

---

### Task 1: Watch/Hover evaluate + capability + basic test

**Files:**
- Modify: `lua_debug.h`, `lua_debug.c`, `dap_session.c`
- Create: `script/test/test_dap_evaluate.py`

**Interfaces:**
- Produces: `lua_debug_evaluate` for watch/hover; initialize flag true; dispatch `evaluate`
- Consumes: `walk_user_frames`, `format_var` / alloc_ref patterns, `dap_session_is_paused` / `paused_L`

- [ ] **Step 1: Write failing test (watch)**

`test_dap_evaluate.py`: start `run_debugee.lua`, BP on `local sum = x + y`, after `stopped`:

```python
c.send_request("evaluate", {
    "expression": "x + y",
    "frameId": frame_id,
    "context": "watch",
})
ev = c.wait_for(lambda m: m.get("command") == "evaluate" and m.get("type") == "response")
assert ev["success"] is True
assert ev["body"]["result"] in ("30", "30.0")  # accept either
# hover
c.send_request("evaluate", {"expression": "x", "frameId": frame_id, "context": "hover"})
...
# initialize capability
# (optional: capture initialize body supportsEvaluateForHovers true)
```

Also assert initialize capability if easy (parse first initialize response).

- [ ] **Step 2: Run — expect fail** (unsupported / success false)

```powershell
cmake --build build/msvc --target luadap
python script/test/test_dap_evaluate.py
```

- [ ] **Step 3: Implement watch/hover path**

In `lua_debug.c`:
1. Resolve `frame_id` via `walk_user_frames`; invalid → error
2. Build env (same as BP cond: locals, upvalues, `__index=_G`) — **no** `__newindex` for watch/hover
3. `loadstring("return ("..expr..")")`, set `_ENV`/upvalue, `pcall`
4. Format first return with shared helper → cJSON body

In `dap_session.c`:
```c
cJSON_AddBoolToObject(caps, "supportsEvaluateForHovers", 1);
// handle_evaluate: if !paused → fail; else lua_debug_evaluate(paused_L, ...)
```

- [ ] **Step 4: Pass watch/hover asserts**

- [ ] **Step 5: Commit**

```bash
git commit -am "feat: DAP evaluate for watch and hover"
```

---

### Task 2: REPL writeback + tables + failure paths + docs

**Files:**
- Expand: `lua_debug.c` evaluate REPL branch
- Expand: `test_dap_evaluate.py`
- Modify: spec status → **已实现**; brief README note if README mentions capabilities

**Interfaces:**
- REPL env stores side table in registry or uservalue: name → `{kind, index}`  
- `__newindex` is a C closure with upvalues: `(L_thread_ref or lightuserdata paused L is wrong — use registry ref to binding table + frame level)`  

**Writeback closure design (concrete):**

When building REPL env, also create `bind` table:
```
bind[name] = { kind=1 local|2 up, idx=n }
```
Push C closure `eval_newindex` with upvalues: `bind`, and store `frame_level` + need function for upvalues via `lua_getinfo` each time from paused L.

Simpler approach matching Lua 5.4: keep `lua_State *L` as the paused state (same thread as pause_loop). Closure upvalues:
1. `bind` table  
2. integer `level`  
On `__newindex(env, k, v)`:
- lookup bind[k]; if local: `lua_getstack(L, level, &ar)` + `lua_pushvalue(v)` + `lua_setlocal(L, &ar, idx)`  
- if up: get func at level + `lua_setupvalue`  
- else: `lua_pushglobaltable`; `lua_setfield` name  

Also update env's raw field so subsequent reads see new value: `lua_rawset` on env.

- [ ] **Step 1: Extend test**

```python
# REPL assign
c.send_request("evaluate", {"expression": "x = 99", "frameId": frame_id, "context": "repl"})
assert success
c.send_request("evaluate", {"expression": "x", "frameId": frame_id, "context": "watch"})
assert result == "99"
# table
c.send_request("evaluate", {"expression": "player", "frameId": frame_id, "context": "watch"})
assert body["variablesReference"] > 0
# bad expr
c.send_request("evaluate", {"expression": "@@@", "context": "watch", "frameId": frame_id})
assert success is False
```

Optional: after continue, evaluate should fail (not paused) — start second scenario or evaluate after continue before process exit.

- [ ] **Step 2: Implement REPL load + writeback**

```c
/* try return (expr); if load fails, luaL_loadstring(L, expr) as statements */
```

- [ ] **Step 3: Pass full evaluate test + quick regression**

```powershell
python script/test/test_dap_evaluate.py
python script/test/test_dap_breakpoint.py
python script/test/test_dap_condition.py
```

- [ ] **Step 4: Mark spec 已实现; commit**

```bash
git commit -am "feat: DAP evaluate REPL writeback; mark evaluate design done"
```

---

### Task 3: Shared env helper + full DAP regression

**Files:**
- Refactor: `eval_breakpoint_condition` to call shared `push_frame_env(L, level, with_newindex)`
- Run full DAP suite from recent plans (luadap + breakpoint + step + disconnect + coro + evaluate)

- [ ] **Step 1: Extract `push_frame_env`**

BP cond uses `with_newindex=0`. Evaluate watch uses 0; REPL uses 1.

- [ ] **Step 2: Full regression**

```powershell
python script/test/test_dap_luadap_handshake.py
python script/test/test_dap_luadap_nowait.py
python script/test/test_dap_luadap_reconnect.py
python script/test/test_dap_breakpoint.py
python script/test/test_dap_step.py
python script/test/test_dap_disconnect.py
python script/test/test_dap_partial_frame.py
python script/test/test_dap_condition.py
python script/test/test_dap_table_cycle.py
python script/test/test_dap_coro.py
python script/test/test_dap_evaluate.py
```

Expected: all exit 0.

- [ ] **Step 3: Commit**

```bash
git commit -am "refactor: share frame env for BP condition and evaluate"
```

---

## Self-Review (plan vs spec)

| Spec item | Task |
|-----------|------|
| supportsEvaluateForHovers | 1 |
| watch/hover | 1 |
| repl + writeback | 2 |
| table variablesReference | 2 |
| failure / not paused | 2 |
| shared env / regression / 已实现 | 2–3 |

No TBD. First-return-only and `result="nil"` locked.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-15-luadap-evaluate.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks  
2. **Inline Execution** — this session with executing-plans and checkpoints  

Which approach?
