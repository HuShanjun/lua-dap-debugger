# asyncsocket General Server/Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Evolve `asyncsocket` into a general async TCP library (multi-client `listen` + `connect`) with Server/Connection Lua objects, rewrite the poll engine, then migrate `luadap` onto the new C API with full DAP regression green.

**Architecture:** Single poll engine owns listen fd + N connection slots + wakeup. Events carry `conn_id` (`ACCEPT`/`OPEN`/`MESSAGE`/`CLOSE`). Lua maps events to Server `on_accept` and Connection callbacks. `luadap` keeps one DAP client conn_id; rejects extra ACCEPT. Soft-reset still keeps listen for F5 re-attach.

**Tech Stack:** C11, WSAPoll/poll, Lua 5.4 C API, existing CMake `asyncsocket`/`asyncsocket_static`/`luadap`, Python tests

**Spec:** `docs/superpowers/specs/2026-08-15-asyncsocket-general-design.md`

## Global Constraints

- Module remains `require("asyncsocket")`; bump `_VERSION` to **0.3.0**
- Lua: Server + Connection; `on_accept`; no primary `listen`-object `on_open`/`on_message`
- One listen per process (second `listen` errors); many connections + many `connect` OK
- `srv:close()` closes listen only; existing conns remain
- Poll thread never calls Lua; main thread `pump()` / `take_events`
- `conn:send` failure → `luaL_error` (match current)
- `luadap`: single DAP client; extra ACCEPT → close new conn; soft-reset keeps listen
- English concise commits
- Build: `cmake --build build/msvc --target asyncsocket luadap` (adjust if build dir differs)

---

## File Structure

| File | Responsibility |
|------|----------------|
| `native/asyncsocket/poll_loop.h` | New engine C API + event types with `conn_id` |
| `native/asyncsocket/poll_loop.c` | Multi-conn poll thread rewrite |
| `native/asyncsocket/asyncsocket.c` | Lua Server/Connection + `listen`/`connect`/`pump` |
| `native/luadap/dap_session.c` | Use new C API; track `dap_conn_id` |
| `script/test/run_asyncsocket_smoke.lua` | `on_accept` rewrite |
| `script/test/test_asyncsocket_smoke.py` | Update tokens if needed |
| `script/test/test_asyncsocket_multi.py` | Two clients |
| `script/test/test_asyncsocket_connect.py` | Outbound connect |
| `README.md` + spec status | Document API; mark 已实现 |

**Locked C API (`poll_loop.h`):**

```c
typedef enum {
    AS_EVT_ACCEPT = 1,  /* inbound ready; conn_id set */
    AS_EVT_OPEN = 2,    /* outbound connect done */
    AS_EVT_MESSAGE = 3,
    AS_EVT_CLOSE = 4
} as_event_type;

typedef struct as_event {
    as_event_type type;
    int conn_id;       /* >0 */
    char *payload;     /* MESSAGE only; else NULL */
    size_t len;
} as_event;

int as_net_init(void);

/* Global V1 engine (one listen max). err buffers optional. */
int as_listen(const char *host, int port, char *err, size_t errlen); /* 0 ok */
int as_connect(const char *host, int port, char *err, size_t errlen); /* returns conn_id>0 or -1 */
int as_conn_send(int conn_id, const void *data, size_t len); /* 0 ok, -1 fail */
void as_conn_close(int conn_id);
void as_server_close(void); /* listen only */
void as_engine_stop(void);  /* stop poll thread, close all fds (process teardown) */

as_event *as_take_events(size_t *out_n);
void as_events_free(as_event *evs, size_t n);
```

**Compat during Task 1 only (optional thin wrappers):** If needed so `luadap` still links mid-branch, implement old `as_socket_*` as wrappers around the new engine with single-client semantics. **Remove wrappers in Task 4** when `luadap` is migrated.

---

### Task 1: Rewrite poll engine (multi-conn + connect C API)

**Files:**
- Replace: `native/asyncsocket/poll_loop.h`, rewrite `poll_loop.c`
- Optionally: temporary `as_socket_*` wrappers in same file or `poll_loop_compat.c`
- Test: small C-less check via existing link — after Task 1, `asyncsocket` Lua may be broken until Task 2; verify with a minimal C-free compile of `asyncsocket_static` + temporary `test` OR keep wrappers and old `asyncsocket.c` compiling

**Preferred Task 1 exit criterion:** `asyncsocket_static` + `luadap` still build and **existing DAP handshake still passes** via compat wrappers (so we do not brick the debugger mid-plan).

**Interfaces:**
- Produces: `as_listen` / `as_connect` / `as_conn_send` / `as_conn_close` / `as_server_close` / `as_take_events` / `as_engine_stop`
- Consumes: existing Windows/POSIX poll patterns from current `poll_loop.c` (reuse wakeup, SO_REUSEADDR, nonblock helpers)

- [ ] **Step 1: Write the new header**

Replace `poll_loop.h` with the locked API above. Document that `conn_id` starts at 1 and is never 0.

- [ ] **Step 2: Implement engine core**

In `poll_loop.c`:
- Global engine: mutex, event queue, poll thread, wakeup pipe/socket
- Connection table: `{ id, fd, send_buf, closing, kind: inbound|outbound }`
- `as_listen`: create listen fd, start thread if needed; reject if listen already active
- `accept` → allocate id → `AS_EVT_ACCEPT`
- `as_connect`: nonblocking connect; on completion `AS_EVT_OPEN` or fail `AS_EVT_CLOSE`
- `recv` → `AS_EVT_MESSAGE`; peer close → `AS_EVT_CLOSE`
- `as_conn_send`: nonblocking + send buffer + POLLOUT flush (same strategy as today)
- `as_server_close`: close listen fd only
- `as_engine_stop`: join thread, close all

- [ ] **Step 3: Compat wrappers for old API**

Map:
```c
as_socket_listen → as_listen + store opaque; take_events filters/adapts
as_socket_send → as_conn_send(current_dap_or_first_conn)
as_socket_stop/destroy → as_server_close + close conns + as_engine_stop as appropriate
```
Keep enough behavior that `dap_session.c` unchanged still passes handshake.

- [ ] **Step 4: Build + handshake smoke**

```powershell
cmake --build build/msvc --target asyncsocket_static luadap
python script/test/test_dap_luadap_handshake.py
```

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git commit -am "feat: multi-conn asyncsocket poll engine with C API"
```

---

### Task 2: Lua Server / Connection API + update smoke

**Files:**
- Rewrite: `native/asyncsocket/asyncsocket.c`
- Modify: `script/test/run_asyncsocket_smoke.lua`, `test_asyncsocket_smoke.py` if needed
- Set `_VERSION = "0.3.0"`

**Interfaces:**
- `as.listen` → server udata with `on_accept`, `close`
- `as.connect` → conn udata with `on_open`/`on_message`/`on_close`/`send`/`close`
- `as.pump` → `as_take_events`, dispatch:
  - ACCEPT → create conn udata, call server `on_accept(conn)`
  - OPEN → conn `on_open`
  - MESSAGE → conn `on_message(chunk)`
  - CLOSE → conn `on_close`, invalidate udata

- [ ] **Step 1: Update smoke Lua to new API (expect fail on old DLL)**

```lua
local as = require("asyncsocket")
local srv = as.listen(host, port)
srv:on_accept(function(conn)
  conn:on_message(function(chunk)
    buf = buf .. chunk
    if buf == "hello" then conn:send(buf) end
  end)
  conn:on_close(function()
    -- relisten path: srv:close(); srv = as.listen(...); ...
  end)
end)
```

Adjust relisten flow to match smoke’s LISTENING / LISTENING2 protocol (read current `run_asyncsocket_smoke.lua` and preserve Python expectations).

- [ ] **Step 2: Run smoke — fail if binding still old**

```powershell
python script/test/test_asyncsocket_smoke.py
```

- [ ] **Step 3: Implement Lua binding**

Registry maps `conn_id` → Lua connection udata (or callback table). Server holds `on_accept` ref.

- [ ] **Step 4: Pass smoke**

- [ ] **Step 5: Commit**

```bash
git commit -am "feat: asyncsocket Server/Connection Lua API 0.3"
```

---

### Task 3: Multi-client + connect tests

**Files:**
- Create: `script/test/run_asyncsocket_multi.lua`, `test_asyncsocket_multi.py`
- Create: `script/test/run_asyncsocket_connect.lua`, `test_asyncsocket_connect.py`

- [ ] **Step 1: Multi-client test**

Server accepts 2 TCP clients; each sends a distinct payload; server echoes per-conn; Python asserts both echoes. Drive with `as.pump()` loop.

- [ ] **Step 2: Connect test**

Process A or same process: `listen` + `connect` to self (or Python acts as one side). Prefer: Lua listens; Lua `connect`s; exchange `"ping"`/`"pong"` via callbacks + pump.

- [ ] **Step 3: Implement fixes until both pass**

- [ ] **Step 4: Commit**

```bash
git commit -am "test: asyncsocket multi-client and connect"
```

---

### Task 4: Migrate luadap + remove compat + full regression + docs

**Files:**
- Modify: `native/luadap/dap_session.c` (and `.h` if needed)
- Remove: old `as_socket_*` wrappers from poll_loop
- Update: `README.md`; mark `docs/superpowers/specs/2026-08-15-asyncsocket-general-design.md` → **已实现**

**luadap mapping:**

```c
/* start */
as_net_init();
as_listen(host, port, err, errlen);
g_sess.dap_conn_id = 0;

/* update take_events */
case AS_EVT_ACCEPT:
  if (g_sess.dap_conn_id == 0) {
    g_sess.dap_conn_id = ev.conn_id;
    g_sess.client_open = 1;
  } else {
    as_conn_close(ev.conn_id); /* reject 2nd debugger */
  }
  break;
case AS_EVT_MESSAGE:
  if (ev.conn_id == g_sess.dap_conn_id) append recv_buf;
  break;
case AS_EVT_CLOSE:
  if (ev.conn_id == g_sess.dap_conn_id) close_pending / soft reset;
  break;

/* send */
as_conn_send(g_sess.dap_conn_id, frame, len);

/* soft reset: clear dap_conn_id, do NOT as_server_close */
/* hard shutdown / start rebuild: as_server_close + close dap conn + as_engine_stop */
```

- [ ] **Step 1: Migrate dap_session + delete compat wrappers**

- [ ] **Step 2: Full regression**

```powershell
cmake --build build/msvc --target asyncsocket luadap
python script/test/test_asyncsocket_smoke.py
python script/test/test_asyncsocket_multi.py
python script/test/test_asyncsocket_connect.py
python script/test/test_dap_luadap_handshake.py
python script/test/test_dap_luadap_nowait.py
python script/test/test_dap_luadap_reconnect.py
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

Expected: all exit 0.

- [ ] **Step 3: Docs**

README Server/Connection examples; spec status **已实现**.

- [ ] **Step 4: Commit**

```bash
git commit -am "feat: migrate luadap to multi-conn asyncsocket; mark general design done"
```

---

## Self-Review (plan vs spec)

| Spec item | Task |
|-----------|------|
| Multi-conn C engine + connect | 1 |
| Server/Connection Lua + on_accept | 2 |
| Multi client + connect tests | 3 |
| luadap migration + soft reset listen | 4 |
| DAP regression + 已实现 | 4 |
| Destroy old single-client accept-reject | 1 (engine) / 4 (luadap policy) |

No TBD. Compat wrappers are explicitly temporary and removed in Task 4.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-15-asyncsocket-general.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks  
2. **Inline Execution** — this session with executing-plans and checkpoints  

Which approach?
