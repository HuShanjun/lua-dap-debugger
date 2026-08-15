# Lua DAP Debugger V1 Design

**Date:** 2026-08-15  
**Status:** Approved for planning  
**Scope:** First working version — host as DAP TCP server, VS Code as thin client bridge

## Goal

Build a Lua debugger where:

1. The C++ host program is the **TCP server**.
2. VS Code attaches as a **remote client**.
3. The Lua runtime speaks **standard DAP** over **luasocket**.
4. Users can set **line breakpoints** and inspect **locals / table members**.

## Decisions (locked)

| Decision | Choice |
|----------|--------|
| DAP location | Lua backend speaks standard DAP (`Content-Length` framing + JSON) |
| VS Code extension | Extremely thin TCP bridge (stdio ↔ TCP); no DAP parsing |
| Startup timing | Host blocks in `listen` until `configurationDone`, then runs business script |
| Implementation shape | Single-file `debugger.lua` (+ vendored `dkjson.lua`) |
| Launch mode | Out of V1 (attach only) |
| Path mapping | Same-machine path normalize only; no remote `pathMappings` |
| JSON | Vendored pure-Lua `dkjson` |

## Architecture

```
┌─────────────┐   DAP over stdio    ┌──────────────────┐   DAP over TCP     ┌────────────────────────────┐
│  VS Code UI │ ◄─────────────────► │ Thin extension   │ ◄────────────────► │ C++ host + debugger.lua    │
└─────────────┘                     │ attach → TCP     │   Content-Length   │ listen → accept → DAP FSM  │
                                    └──────────────────┘                    │ debug.sethook bp/step/vars │
                                                                            └────────────────────────────┘
```

### Component responsibilities

| Component | Responsibility |
|-----------|----------------|
| C++ `main` | Init Lua (sol2), set `package.path` / `package.cpath`, call `debugger.listen`, then run sample script |
| `script/lua-runtime/debugger.lua` | TCP server, DAP framing/dispatch, hook, breakpoints, stepping, stack/variables |
| `script/lua-runtime/dkjson.lua` | Pure Lua JSON encode/decode |
| VS Code extension | Byte-forward stdio ↔ TCP for attach config only |
| `script/sample/main.lua` | Demo script with locals + nested tables for manual verification |

## Connection sequence (V1)

1. Host starts → `require("lua-runtime.debugger").listen("127.0.0.1", 8172)`.
2. Server binds and **blocks on `accept`**.
3. User presses F5 with Attach config → extension connects TCP.
4. DAP handshake:
   - `initialize` request → response with capabilities → `initialized` event
   - `attach` request → response
   - `setBreakpoints` (zero or more) → response
   - `configurationDone` → response
5. `listen` returns to C++ host.
6. Host runs business script; line hook is already installed.
7. On breakpoint/step hit → send `stopped` event → enter synchronous DAP read loop inside the hook until continue/step.
8. On script end or `disconnect` → send `terminated` (as applicable), remove hook, close sockets.

## DAP transport

Standard Debug Adapter Protocol framing:

```
Content-Length: <n>\r\n
\r\n
<json-body of exactly n bytes>
```

Messages are DAP JSON objects (`type`: `request` | `response` | `event`).

## DAP surface (V1)

### Requests handled

| Request | Behavior |
|---------|----------|
| `initialize` | Return capabilities: `supportsConfigurationDoneRequest=true`; other advanced caps false/omitted |
| `attach` | Mark session connected; do **not** start user script (host starts it after `listen` returns) |
| `setBreakpoints` | Replace all breakpoints for the given source path; normalize path; return verified breakpoints |
| `setExceptionBreakpoints` | No-op success response (VS Code often sends this; V1 does not stop on errors) |
| `configurationDone` | Complete handshake; unblock `listen` |
| `threads` | Return a single synthetic thread `{ id = 1, name = "main" }` |
| `continue` | Clear step mode; resume; `allThreadsContinued = true` |
| `next` | Step over (depth-aware) |
| `stepIn` | Step into (stop on next line event) |
| `stepOut` | Step out (stop when stack depth decreases) |
| `stackTrace` | Build frames via `debug.getinfo` for threadId 1 |
| `scopes` | Expose `Locals` and `Upvalues` scopes with `variablesReference` |
| `variables` | Resolve locals / table members / upvalues by reference |
| `disconnect` / `terminate` | Remove hook, close client/server, allow host to finish or exit cleanly |

All responses/events carry monotonically increasing `seq`. Unknown requests get a failure response (`success=false`) rather than hanging the client.

### Events emitted

| Event | When |
|-------|------|
| `initialized` | After successful `initialize` response |
| `stopped` | Breakpoint or step hit (`reason`: `breakpoint` \| `step`, `threadId`: 1) |
| `terminated` | Debug session ending / script finished under debug |

### Explicitly out of V1

- `launch` request / launching Lua from the extension
- Conditional breakpoints, logpoints, hit-count conditions
- `evaluate` / Watch expressions
- Coroutine debugging
- Multi-client, reconnect policy
- Remote `pathMappings`
- Set variable / set expression

## Debug runtime design

### Hook

- Install `debug.sethook(hook, "lcr")` before user script runs (at latest before `configurationDone` returns).
- On each line event:
  1. Normalize current source path + line.
  2. If breakpoint table hits → `pause("breakpoint")`.
  3. Else if step mode satisfied → `pause("step")`.

### Pause model (critical)

On pause, **do not** yield to a separate debugger coroutine that leaves the debugee stack.

Instead, inside the hook:

1. Send `stopped` event.
2. Enter a **synchronous** socket read/dispatch loop.
3. Serve `stackTrace` / `scopes` / `variables` against the live stack.
4. Exit the loop only on `continue` / `next` / `stepIn` / `stepOut` / disconnect.

This keeps `debug.getlocal` valid for the paused frame.

### Path normalization

For breakpoint keys and stack frames:

1. Strip leading `@` from `debug.getinfo` source.
2. Convert `\` to `/`.
3. Lowercase Windows drive letter when present.
4. Optional: trim trailing slashes.

V1 assumes VS Code and host see the **same machine filesystem** (no path mapping table).

### Variables

- **Locals:** `debug.getlocal(level, i)`; skip `(temporary)` / `(*temporary*)` names as appropriate for Lua version.
- **Upvalues:** `debug.getupvalue` on the frame function; shown as a separate scope.
- **Tables:** assign an incrementing `variablesReference`, cache the table object; on expand, iterate `pairs` and emit children; nested tables get new refs.
- **Scalars:** stringified `value`, `variablesReference = 0`.
- Clear variable ref cache when leaving pause (or on each new stop) to avoid stale refs.

### Stepping

| Command | Rule |
|---------|------|
| `stepIn` | Stop on next line event |
| `next` (over) | Record depth at resume; stop on line when depth ≤ recorded depth |
| `stepOut` | Record depth; stop when depth < recorded depth |
| `continue` | No step flag; only breakpoints stop |

Skip debugger's own internal frames when computing depth / stackTrace.

## Host integration

`main/main.cpp` responsibilities after Lua init:

```cpp
// package.path already includes script/
lua.script(R"(
  local dbg = require("lua-runtime.debugger")
  dbg.listen("127.0.0.1", 8172)
)");
RunFile(lua, "E:/demo/lua-dap-debugger/script/sample/main.lua");
```

Notes:

- Prefer configurable host/port via env (`LUADAP_HOST`, `LUADAP_PORT`) with defaults `127.0.0.1` / `8172`.
- Hardcoded sample path is acceptable for V1 demo; later can take argv.
- `package.cpath` must resolve luasocket (`socket.dll` under `bin/`).

Public Lua API:

```lua
local dbg = require("lua-runtime.debugger")
dbg.listen(host, port)  -- blocks until configurationDone
-- optional later: dbg.stop()
```

## VS Code thin bridge

Rewrite `vscode-extension` to:

1. Register debug type `lua-dap`.
2. Provide `DebugAdapterDescriptorFactory` that starts a small Node script (or inline adapter process).
3. On `attach`, connect TCP to `host`/`port` from launch.json.
4. Bidirectionally forward raw bytes between VS Code DAP stdio and the TCP socket.
5. Do **not** parse DAP JSON.

`launch.json` default configuration:

```json
{
  "type": "lua-dap",
  "request": "attach",
  "name": "Lua DAP: Attach",
  "host": "127.0.0.1",
  "port": 8172
}
```

Launch configurations may remain as stubs or be removed from V1 docs.

## File changes

| Path | Action |
|------|--------|
| `script/lua-runtime/debugger.lua` | Rewrite as standard DAP + luasocket server |
| `script/lua-runtime/dkjson.lua` | Add vendored dkjson (MIT) |
| `main/main.cpp` | Call `dbg.listen` before running sample |
| `vscode-extension/src/*` | Rewrite as thin TCP bridge |
| `vscode-extension/package.json` | Keep attach attributes; simplify scripts if needed |
| `.vscode/launch.json` | Default to Attach |
| `script/sample/main.lua` | Add table fields for member inspection demo |
| `README.md` | Document real V1 attach workflow |

## Error handling (V1)

- Bind failure: print clear error and abort host start.
- Client disconnect during pause: remove hook, break out of pause loop, let script continue or exit depending on disconnect `terminateDebuggee` (default: continue script without debugger).
- Malformed DAP frame / JSON: log and close session rather than crash host if possible (`pcall` around decode/dispatch).
- Missing luasocket / dkjson: fail fast with require error message.

## Testing / acceptance

Manual acceptance criteria:

1. Start `main.exe` → console shows listening on `127.0.0.1:8172` and waits.
2. VS Code F5 Attach → handshake succeeds; sample script starts.
3. Breakpoint on a line in `sample/main.lua` hits at the correct line.
4. Variables view shows locals; expanding a table shows member fields.
5. Continue / Step Over / Step Into / Step Out work.
6. Stop debugging disconnects cleanly without leaving a wedged host (or host exits predictably).

Optional later: a Python/Node TCP client that speaks DAP for automated smoke tests.

## Non-goals reminder

This V1 is intentionally narrow: attach-only, same-machine paths, breakpoints + stepping + locals/table members, thin VS Code bridge. Anything beyond that waits for a later version.
