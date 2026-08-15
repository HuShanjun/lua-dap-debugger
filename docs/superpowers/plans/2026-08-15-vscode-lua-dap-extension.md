# VS Code lua-dap Extension + lua-runner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a VS Code `lua-dap` debugger (Attach + Launch) that uses a new `lua-runner` executable with Lua and `luadap` linked in; replace the `type:node` + `debugServer` workaround.

**Architecture:** Thin TypeScript extension returns `vscode.DebugAdapterServer(port, host)`. Launch spawns `lua-runner --host … --port … -- program.lua [args]`. `lua-runner` statically links `luadap` sources (via `luadap_static`), registers `luaopen_luadap`, calls `dap.start(..., true)`, runs the file, pumps `dap.update` (and `update` if present). Attach only connects TCP to an already-listening host.

**Tech Stack:** C11, CMake, existing `luadap` / `asyncsocket_static` / `liblua`, TypeScript VS Code extension API, Python DAP tests

**Spec:** `docs/superpowers/specs/2026-08-15-vscode-lua-dap-extension-design.md`

## Global Constraints

- Launch **only** via `lua-runner` (no system `luaexe` + `luadap.dll` Launch path)
- Attach = TCP to existing DAP listen port (no PID inject)
- Thin extension + `DebugAdapterServer`; do **not** reimplement DAP in Node
- Builtin binary: Windows x64 `lua-runner.exe`; override via `luadap.runnerPath` / launch `runnerPath`
- Do not change DAP protocol semantics in `luadap`
- Replace abandoned `vscode-extension/src/*.ts` (old Executor/bridge stubs are invalid)
- English concise commits
- Build: `cmake --build build/msvc --target lua-runner` (or project’s usual build dir)

---

## File Structure

| File | Responsibility |
|------|----------------|
| `native/luadap/CMakeLists.txt` | Add `luadap_static` (same sources; `LUADAP_STATIC`) |
| `native/luadap/luadap.c` | Guard `LUADAP_API` when static |
| `tools/lua-runner/lua_runner.c` | CLI host: parse args, open libs, requiref luadap, start, dofile, pump |
| `tools/lua-runner/CMakeLists.txt` | `add_executable(lua-runner …)` → `bin/` |
| `CMakeLists.txt` | `add_subdirectory(tools/lua-runner)` |
| `script/test/run_debugee_runner.lua` | Minimal script for runner smoke (breakpoint-friendly) |
| `script/test/test_dap_runner_handshake.py` | Spawn `lua-runner`, DAP handshake |
| `vscode-extension/package.json` | `type: lua-dap`, launch/attach attrs, settings |
| `vscode-extension/src/extension.ts` | Activate + `DebugAdapterDescriptorFactory` |
| `vscode-extension/src/launch.ts` | Resolve runner, free port, spawn, wait TCP, kill on end |
| `vscode-extension/tsconfig.json` | Compile to `out/` |
| Delete or replace | `vscode-extension/src/debugger.ts` (obsolete) |
| `.vscode/launch.json` | `lua-dap` Launch / Attach configs |
| `README.md` | Extension + runner docs |
| Spec | Mark **已实现** when done |

**Locked CLI:**

```text
lua-runner [--host HOST] [--port PORT] [--] <program.lua> [script_args...]
```

Defaults: host `LUADAP_HOST` or `127.0.0.1`; port `LUADAP_PORT` or `8172`.

**Locked C registration:**

```c
/* After luaL_openlibs(L): */
luaL_requiref(L, "luadap", luaopen_luadap, 1);
lua_pop(L, 1);
/* Then get global "luadap", call start/update via Lua C API or lua_getglobal + pcall */
```

---

### Task 1: `luadap_static` + `lua-runner` + handshake test

**Files:**
- Modify: `native/luadap/CMakeLists.txt`, `native/luadap/luadap.c`
- Create: `tools/lua-runner/lua_runner.c`, `tools/lua-runner/CMakeLists.txt`
- Modify: root `CMakeLists.txt`
- Create: `script/test/run_debugee_runner.lua`, `script/test/test_dap_runner_handshake.py`

**Interfaces:**
- Produces: target `lua-runner` in `bin/`; `luaopen_luadap` available when linking `luadap_static`
- Consumes: `dap_session_start` / `update` via Lua module API already exported by `luaopen_luadap`

- [ ] **Step 1: Write failing test**

`script/test/run_debugee_runner.lua`:

```lua
-- No require("luadap"): runner preloads it.
local x, y = 10, 20
local sum = x + y
print("sum", sum)
print("DEBUGEE_DONE")
io.stdout:flush()
```

`script/test/test_dap_runner_handshake.py` (mirror `test_dap_luadap_handshake.py`, but spawn runner):

```python
ROOT = Path(__file__).resolve().parents[2]
PORT = 18290
RUNNER = ROOT / "bin" / "lua-runner.exe"  # also try bin/Debug/
DEBUGEE = ROOT / "script" / "test" / "run_debugee_runner.lua"

def find_runner():
    for p in [ROOT / "bin" / "lua-runner.exe", ROOT / "bin" / "Debug" / "lua-runner.exe"]:
        if p.exists():
            return str(p)
    raise SystemExit("lua-runner not found; build target lua-runner first")

# Popen: [runner, "--host", "127.0.0.1", "--port", str(PORT), "--", str(DEBUGEE)]
# Then initialize / attach / configurationDone like test_dap_luadap_handshake.py
# Assert DEBUGEE_DONE in stdout; print("runner handshake ok")
```

- [ ] **Step 2: Run test — expect fail (no runner)**

```
python script/test/test_dap_runner_handshake.py
```

Expected: exit non-zero / `lua-runner not found`

- [ ] **Step 3: Add `luadap_static`**

In `native/luadap/CMakeLists.txt`, after the SHARED target, add a STATIC library from the same sources with `LUADAP_STATIC` define; link `asyncsocket_static`, `lua_compat`, `liblua`, `ws2_32` on Windows.

In `luadap.c`:

```c
#ifdef LUADAP_STATIC
#define LUADAP_API
#elif defined(_WIN32)
#define LUADAP_API __declspec(dllexport)
#else
#define LUADAP_API __attribute__((visibility("default")))
#endif
```

Keep SHARED `luadap` for existing `require("luadap")` hosts/tests.

- [ ] **Step 4: Implement `lua_runner.c`**

Parse `--host` / `--port` / `--` / program / script args. Create state, `luaL_openlibs`, `luaL_requiref(L, "luadap", luaopen_luadap, 1)`, set `arg` table, call `luadap.start(host, port, true)`, print `LISTEN_DONE\n` and flush **after** start returns (i.e. after configurationDone), `luaL_dofile(program)`, then:

```c
/* if global update is function: loop update(n) + luadap.update()
 * else: loop luadap.update() with short sleep until update errors / optional disconnect
 */
```

Use `lua_getglobal` + `lua_pcall` for `start`/`update` to match module table API. On Windows sleep via `Sleep(10)`.

Declare `extern int luaopen_luadap(lua_State *L);` or include a tiny header.

- [ ] **Step 5: CMake wire-up**

`tools/lua-runner/CMakeLists.txt`:

```cmake
add_executable(lua-runner lua_runner.c)
target_include_directories(lua-runner PRIVATE
  ${LUA_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/native/luadap
  ${CMAKE_SOURCE_DIR}/native/common)
target_link_libraries(lua-runner PRIVATE luadap_static liblua)
# WIN32: ws2_32 already via luadap_static transitive or add explicitly
```

Root: `add_subdirectory(tools/lua-runner)`.

- [ ] **Step 6: Build + pass test**

```
cmake --build build/msvc --target lua-runner
python script/test/test_dap_runner_handshake.py
```

Expected: `runner handshake ok`

- [ ] **Step 7: Commit**

```bash
git add native/luadap tools/lua-runner CMakeLists.txt script/test/run_debugee_runner.lua script/test/test_dap_runner_handshake.py
git commit -m "feat: add lua-runner with statically linked luadap"
```

---

### Task 2: VS Code extension Attach via `DebugAdapterServer`

**Files:**
- Replace: `vscode-extension/package.json`, `vscode-extension/src/extension.ts`, `vscode-extension/tsconfig.json`
- Delete: `vscode-extension/src/debugger.ts` (obsolete bridge)
- Create: `vscode-extension/src/launch.ts` (stub exports ok; Launch filled in Task 3)

**Interfaces:**
- Produces: `registerDebugAdapterDescriptorFactory('lua-dap', …)` returning `DebugAdapterServer` for `request === 'attach'`
- Consumes: none from Task 1 at runtime for Attach

- [ ] **Step 1: Rewrite `package.json` contributes**

- `type: lua-dap`, languages `["lua"]`
- attach: `host`, `port`
- launch attrs can be declared now (`program`, `args`, `cwd`, `host`, `port`, `runnerPath`) even if Launch logic is Task 3
- configuration defaults for Attach / Launch snippets
- settings: `luadap.runnerPath`, `luadap.defaultPort`
- `main`: `./out/extension.js`
- scripts: `compile` with `tsc`
- devDependency `@types/vscode`, `typescript`

- [ ] **Step 2: Implement Attach factory**

`extension.ts`:

```typescript
import * as vscode from 'vscode';
import { createDebugAdapterDescriptor } from './launch';

export function activate(context: vscode.ExtensionContext) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory('lua-dap', {
      createDebugAdapterDescriptor(session) {
        return createDebugAdapterDescriptor(context, session);
      },
    })
  );
}
```

`launch.ts` (Attach path only for this task):

```typescript
export async function createDebugAdapterDescriptor(
  context: vscode.ExtensionContext,
  session: vscode.DebugSession
): Promise<vscode.DebugAdapterDescriptor> {
  const cfg = session.configuration;
  if (cfg.request === 'attach') {
    const host = cfg.host || vscode.workspace.getConfiguration('luadap').get('defaultHost', '127.0.0.1');
    const port = cfg.port ?? vscode.workspace.getConfiguration('luadap').get('defaultPort', 8172);
    return new vscode.DebugAdapterServer(port as number, host as string);
  }
  throw new Error('Launch not implemented yet'); // replaced in Task 3
}
```

Use `defaultPort` only if you add that setting key consistently with the spec (`luadap.defaultPort`). Host default hardcode `127.0.0.1` if no setting.

- [ ] **Step 3: `npm install` + `npm run compile` in `vscode-extension/`**

Expected: `out/extension.js` exists; no tsc errors.

- [ ] **Step 4: Manual Attach check (document in report)**

1. Start `main.exe` or `lua run_debugee_luadap.lua` listening on 8172  
2. Extension Development Host with a launch config `type: lua-dap`, `request: attach`, `port: 8172`  
3. Confirm session connects (or skip if headless and note “manual”)  

Automated substitute: no change to Python attach tests required for this task.

- [ ] **Step 5: Commit**

```bash
git add vscode-extension
git commit -m "feat: VS Code lua-dap Attach via DebugAdapterServer"
```

---

### Task 3: Extension Launch (spawn `lua-runner`)

**Files:**
- Modify: `vscode-extension/src/launch.ts`, `vscode-extension/package.json` (if needed)
- Create: script to copy `bin/lua-runner.exe` → `vscode-extension/bin/win32-x64/lua-runner.exe` (e.g. `vscode-extension/scripts/copy-runner.ps1` or CMake `POST_BUILD`)

**Interfaces:**
- Consumes: Task 1 `lua-runner` CLI
- Produces: Launch path returns `DebugAdapterServer` after TCP ready; kills child on session end

- [ ] **Step 1: Resolve runner path**

Order: `configuration.runnerPath` → `luadap.runnerPath` setting → `context.asAbsolutePath('bin/win32-x64/lua-runner.exe')`.

- [ ] **Step 2: Free port helper**

```typescript
import * as net from 'net';
export function getFreePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.listen(0, '127.0.0.1', () => {
      const addr = s.address();
      if (addr && typeof addr === 'object') {
        const p = addr.port;
        s.close(() => resolve(p));
      } else reject(new Error('no port'));
    });
    s.on('error', reject);
  });
}
```

- [ ] **Step 3: Spawn + wait TCP**

For `request === 'launch'`:
1. Resolve `program` (required), `args`, `cwd`, `host` (default `127.0.0.1`), `port` (or `getFreePort()`)
2. `cp.spawn(runner, ['--host', host, '--port', String(port), '--', program, ...args], { cwd })`
3. Pipe stdout/stderr to Debug Console via `vscode.debug.activeDebugConsole?.appendLine` or Output channel
4. Poll `net.connect(port, host)` until success (timeout ~10s) then `return new vscode.DebugAdapterServer(port, host)`
5. Track child; on `vscode.debug.onDidTerminateDebugSession` matching session, `child.kill()`

- [ ] **Step 4: Copy runner into extension bin**

After building `lua-runner`, copy to `vscode-extension/bin/win32-x64/`. Add `.gitignore` entry for the exe if desired; document copy step in README (Task 4).

Optional CMake `add_custom_command(TARGET lua-runner POST_BUILD …)`.

- [ ] **Step 5: Compile extension + smoke**

```
cd vscode-extension && npm run compile
```

Manual: Launch `${file}` on `script/test/run_debugee_runner.lua` with breakpoint on `sum` line.

- [ ] **Step 6: Commit**

```bash
git add vscode-extension tools/lua-runner CMakeLists.txt
git commit -m "feat: VS Code Launch spawns lua-runner"
```

---

### Task 4: Repo `launch.json`, README, mark spec done

**Files:**
- Modify: `.vscode/launch.json`, `README.md`
- Modify: `docs/superpowers/specs/2026-08-15-vscode-lua-dap-extension-design.md` (状态 → 已实现)
- Optional: `.vscode/extensions.json` / compound config for Extension Host — only if needed for local F5 of the extension

**Interfaces:**
- Consumes: Tasks 1–3 deliverables

- [ ] **Step 1: Update `.vscode/launch.json`**

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Lua DAP: Launch current file",
      "type": "lua-dap",
      "request": "launch",
      "program": "${file}",
      "cwd": "${workspaceFolder}",
      "runnerPath": "${workspaceFolder}/bin/lua-runner.exe"
    },
    {
      "name": "Lua DAP: Attach",
      "type": "lua-dap",
      "request": "attach",
      "host": "127.0.0.1",
      "port": 8172
    }
  ]
}
```

Remove or comment old `type: node` + `debugServer` entry; README notes it deprecated.

- [ ] **Step 2: README section**

Document: build `lua-runner`; copy/path for extension; Launch current file; Attach to host; ABI note no longer needed for Launch; Attach hosts still embed `luadap`.

- [ ] **Step 3: Mark spec 已实现**

- [ ] **Step 4: Regression**

```
python script/test/test_dap_runner_handshake.py
python script/test/test_dap_luadap_handshake.py
python script/test/test_dap_evaluate.py
```

Expected: all exit 0.

- [ ] **Step 5: Commit**

```bash
git add .vscode/launch.json README.md docs/superpowers/specs/2026-08-15-vscode-lua-dap-extension-design.md
git commit -m "docs: wire lua-dap extension configs; mark design done"
```

---

## Spec coverage checklist

| Spec requirement | Task |
|------------------|------|
| `lua-runner` CLI + linked luadap | 1 |
| Runner handshake test | 1 |
| Attach `DebugAdapterServer` | 2 |
| Launch spawn runner + free port | 3 |
| Builtin / `runnerPath` | 3 |
| `launch.json` + README | 4 |
| No `luaexe` Launch path | 2–3 (package.json attrs) |
| Deprecate node+debugServer | 4 |

## Consistency notes

- Module API remains `start` / `update` / `track` from `luaopen_luadap`.
- SHARED `luadap.dll` kept for Attach hosts and existing Python suite.
- Old `debugger.ts` / `DebugAdapterExecutor` APIs must not remain.
