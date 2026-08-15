# Lua DAP 调试器 V1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 C++ 宿主通过 luasocket 提供标准 DAP TCP 服务，VS Code 用 `debugServer` 直连后能断点、步进、查看 locals/table 成员。

**Architecture:** Lua `debugger.lua` 作为 DAP adapter（Content-Length 帧）；宿主 `listen` 阻塞到 `configurationDone` 再跑业务脚本；命中断点时在 hook 内同步处理 DAP；VS Code 不写扩展，只用 `type:node` + `debugServer`。

**Tech Stack:** Lua 5.4、luasocket、dkjson、sol2/C++ 宿主、Python3（冒烟测试）、VS Code `debugServer`

**Spec:** `docs/superpowers/specs/2026-08-15-lua-dap-debugger-design.md`

## Global Constraints

- DAP 必须是标准 `Content-Length` 帧，禁止旧版 `\n` 自定义 JSON 协议
- VS Code 接入只用 `debugServer`，V1 不实现/不依赖 `vscode-extension/`
- `listen` 必须阻塞到 `configurationDone` 才返回
- 暂停必须在 hook 内同步读循环，保证 `debug.getlocal` 有效
- 同机路径规范化即可，不做 pathMappings
- 端口默认 `8172`，可用环境变量 `LUADAP_HOST` / `LUADAP_PORT`
- 提交信息用英文 concise style；不擅自 force-push / 改 git config

---

## File Structure

| 文件 | 职责 |
|------|------|
| `script/lua-runtime/dkjson.lua` | 纯 Lua JSON |
| `script/lua-runtime/debugger.lua` | DAP server + hook + 断点/步进/变量（重写） |
| `script/test/dap_client.py` | 可复用的 DAP TCP 客户端小库 |
| `script/test/test_dap_handshake.py` | 握手冒烟测试 |
| `script/test/test_dap_breakpoint.py` | 断点 + 变量冒烟测试（经独立 lua 入口） |
| `script/test/run_debugee.lua` | 测试用：listen + 跑一段带 table 的脚本 |
| `main/main.cpp` | 宿主接入 `dbg.listen` |
| `.vscode/launch.json` | `debugServer: 8172` |
| `script/sample/main.lua` | 演示 locals + nested table |
| `README.md` | V1 使用说明 |

---

### Task 1: 引入 dkjson

**Files:**
- Create: `script/lua-runtime/dkjson.lua`
- Test: 用已编译的 `bin/lua.exe`（若无则用 `build/msvc/...` 或系统 `lua`）做一次 encode/decode

**Interfaces:**
- Produces: `local json = require("lua-runtime.dkjson")` → `json.encode(tbl)` / `json.decode(str)`

- [ ] **Step 1: 下载 dkjson 到仓库**

```powershell
# 在仓库根目录执行
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/LuaDist/dkjson/master/dkjson.lua" -OutFile "script/lua-runtime/dkjson.lua"
# 若 GitHub 不可达，改用：http://dkolf.de/dkjson-lua/dkjson-2.8.lua（或官网最新版）
```

确认文件头含 David Kolf / MIT 许可声明。若下载的是裸模块且模块名为 `dkjson`，保持文件名 `dkjson.lua` 即可；`require("lua-runtime.dkjson")` 在 `package.path` 含 `script/?.lua` 时解析为 `script/lua-runtime/dkjson.lua`。

- [ ] **Step 2: 用 lua 验证编解码**

先确认解释器路径（按本机实际二选一）：

```powershell
Get-ChildItem -Recurse -Filter lua.exe bin,build | Select-Object -First 5 FullName
```

然后：

```powershell
$lua = "<上一步找到的 lua.exe 完整路径>"
& $lua -e "package.path='script/?.lua;'..package.path; local j=require('lua-runtime.dkjson'); local s=j.encode({a=1}); local t=j.decode(s); assert(t.a==1); print('dkjson ok')"
```

Expected: 打印 `dkjson ok`

- [ ] **Step 3: Commit**

```powershell
git add script/lua-runtime/dkjson.lua
git commit -m "chore: vendor dkjson for DAP JSON framing"
```

---

### Task 2: DAP 传输层 + listen 握手

**Files:**
- Rewrite: `script/lua-runtime/debugger.lua`（先只实现网络 + 握手，hook/变量可 stub）
- Create: `script/test/dap_client.py`
- Create: `script/test/test_dap_handshake.py`

**Interfaces:**
- Produces:
  - `M.listen(host?: string, port?: number)` — 阻塞到 `configurationDone`
  - 内部：`send_message(obj)`、`read_message()`、`handle_request(req)`
- Consumes: `require("socket")`, `require("lua-runtime.dkjson")`

- [ ] **Step 1: 写 Python DAP 客户端辅助库**

创建 `script/test/dap_client.py`：

```python
import json
import socket


class DapClient:
    def __init__(self, host="127.0.0.1", port=8172, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.seq = 0

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send_request(self, command, arguments=None):
        self.seq += 1
        body = {
            "seq": self.seq,
            "type": "request",
            "command": command,
            "arguments": arguments or {},
        }
        data = json.dumps(body).encode("utf-8")
        header = f"Content-Length: {len(data)}\r\n\r\n".encode("ascii")
        self.sock.sendall(header + data)
        return self.seq

    def read_message(self):
        while True:
            idx = self.buf.find(b"\r\n\r\n")
            if idx >= 0:
                header = self.buf[:idx].decode("ascii", errors="replace")
                length = None
                for line in header.split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        length = int(line.split(":", 1)[1].strip())
                        break
                if length is None:
                    raise RuntimeError(f"missing Content-Length: {header!r}")
                start = idx + 4
                if len(self.buf) >= start + length:
                    body = self.buf[start : start + length]
                    self.buf = self.buf[start + length :]
                    return json.loads(body.decode("utf-8"))
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("socket closed")
            self.buf += chunk

    def wait_for(self, pred, limit=20):
        for _ in range(limit):
            msg = self.read_message()
            if pred(msg):
                return msg
        raise TimeoutError("wait_for exceeded limit")
```

- [ ] **Step 2: 写失败的握手测试**

创建 `script/test/test_dap_handshake.py`：

```python
import subprocess
import sys
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18172


def find_lua():
    for p in [
        ROOT / "bin" / "lua.exe",
        ROOT / "bin" / "Debug" / "lua.exe",
    ]:
        if p.exists():
            return str(p)
    # fallback: search build
    hits = list(ROOT.glob("**/lua.exe"))
    if hits:
        return str(hits[0])
    raise SystemExit("lua.exe not found; build the project first")


def main():
    lua = find_lua()
    script = f"""
package.path = [[{ROOT.as_posix()}/script/?.lua;]] .. package.path
package.cpath = [[{ROOT.as_posix()}/bin/?.dll;{ROOT.as_posix()}/bin/Debug/?.dll;]] .. package.cpath
local dbg = require("lua-runtime.debugger")
dbg.listen("127.0.0.1", {PORT})
print("LISTEN_DONE")
"""
    proc = subprocess.Popen(
        [lua, "-e", script],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.5)
    try:
        c = DapClient("127.0.0.1", PORT, timeout=3.0)
        c.send_request("initialize", {"adapterID": "lua-dap", "pathFormat": "path", "linesStartAt1": True, "columnsStartAt1": True})
        init_resp = c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize")
        assert init_resp.get("success") is True, init_resp
        ev = c.wait_for(lambda m: m.get("type") == "event" and m.get("event") == "initialized")
        assert ev["event"] == "initialized"
        c.send_request("attach", {})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "attach")
        c.send_request("setExceptionBreakpoints", {"filters": []})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "setExceptionBreakpoints")
        c.send_request("configurationDone", {})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "configurationDone")
        # listen should return and print
        out, _ = proc.communicate(timeout=3)
        assert "LISTEN_DONE" in out, out
        print("handshake ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        try:
            c.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: 运行测试确认当前失败**

```powershell
cd script/test
python test_dap_handshake.py
```

Expected: 连接失败或 `require` 失败（旧 `debugger.lua` 不是标准 DAP / API 不对）。

- [ ] **Step 4: 重写 `debugger.lua` 最小握手实现**

用下面结构整体替换 `script/lua-runtime/debugger.lua`（本 Task 只保证握手；hook 先装空实现）：

```lua
local socket = require("socket")
local json = require("lua-runtime.dkjson")

local M = {}

local state = {
    host = "127.0.0.1",
    port = 8172,
    server = nil,
    client = nil,
    seq = 0,
    configured = false,
    breakpoints = {}, -- [norm_path] = { [line] = true }
    var_refs = {},
    next_ref = 1000,
    step = nil, -- nil | "in" | "over" | "out"
    step_depth = 0,
    paused = false,
    resume_cmd = nil,
}

local function env_host_port(host, port)
    host = host or os.getenv("LUADAP_HOST") or "127.0.0.1"
    port = tonumber(port or os.getenv("LUADAP_PORT") or 8172)
    return host, port
end

local function normalize_path(path)
    if not path or path == "" then return path end
    if path:sub(1, 1) == "@" then path = path:sub(2) end
    path = path:gsub("\\", "/")
    path = path:gsub("^([A-Za-z]):", function(d) return d:lower() .. ":" end)
    while path:sub(-1) == "/" do path = path:sub(1, -2) end
    return path
end

local function send_raw(obj)
    state.seq = state.seq + 1
    obj.seq = state.seq
    local body = json.encode(obj)
    local frame = string.format("Content-Length: %d\r\n\r\n%s", #body, body)
    local ok, err = state.client:send(frame)
    if not ok then error("send failed: " .. tostring(err)) end
end

local function send_response(req, body, success, message)
    send_raw({
        type = "response",
        request_seq = req.seq,
        success = success ~= false,
        command = req.command,
        message = message,
        body = body or {},
    })
end

local function send_event(event, body)
    send_raw({
        type = "event",
        event = event,
        body = body or {},
    })
end

local function read_message()
    local header = ""
    while true do
        local line, err = state.client:receive("*l")
        if not line then error("recv header failed: " .. tostring(err)) end
        if line == "" then break end
        header = header .. line .. "\n"
    end
    local len = header:match("[Cc]ontent%-[Ll]ength:%s*(%d+)")
    if not len then error("missing Content-Length") end
    local body, err = state.client:receive(tonumber(len))
    if not body then error("recv body failed: " .. tostring(err)) end
    local obj, _, jerr = json.decode(body)
    if not obj then error("json decode: " .. tostring(jerr)) end
    return obj
end

local function handle_initialize(req)
    send_response(req, {
        supportsConfigurationDoneRequest = true,
        supportsSetVariable = false,
        supportsConditionalBreakpoints = false,
        supportsEvaluateForHovers = false,
    })
    send_event("initialized")
end

local function handle_attach(req)
    send_response(req, {})
end

local function handle_threads(req)
    send_response(req, { threads = { { id = 1, name = "main" } } })
end

local function handle_set_exception_breakpoints(req)
    send_response(req, {})
end

local function handle_set_breakpoints(req)
    local args = req.arguments or {}
    local src = args.source or {}
    local path = normalize_path(src.path or "")
    state.breakpoints[path] = {}
    local out = {}
    for _, bp in ipairs(args.breakpoints or {}) do
        local line = bp.line
        state.breakpoints[path][line] = true
        out[#out + 1] = { line = line, verified = true }
    end
    send_response(req, { breakpoints = out })
end

local function handle_configuration_done(req)
    send_response(req, {})
    state.configured = true
end

local function handle_disconnect(req)
    send_response(req, {})
    state.resume_cmd = "disconnect"
    state.configured = true
    state.paused = false
end

-- stubs filled in later tasks
local function handle_continue(req) send_response(req, { allThreadsContinued = true }); state.resume_cmd = "continue"; state.step = nil; state.paused = false end
local function handle_next(req) send_response(req, {}); state.resume_cmd = "next"; state.paused = false end
local function handle_step_in(req) send_response(req, {}); state.resume_cmd = "stepIn"; state.paused = false end
local function handle_step_out(req) send_response(req, {}); state.resume_cmd = "stepOut"; state.paused = false end
local function handle_stack_trace(req) send_response(req, { stackFrames = {}, totalFrames = 0 }) end
local function handle_scopes(req) send_response(req, { scopes = {} }) end
local function handle_variables(req) send_response(req, { variables = {} }) end

local handlers = {
    initialize = handle_initialize,
    attach = handle_attach,
    threads = handle_threads,
    setExceptionBreakpoints = handle_set_exception_breakpoints,
    setBreakpoints = handle_set_breakpoints,
    configurationDone = handle_configuration_done,
    continue = handle_continue,
    next = handle_next,
    stepIn = handle_step_in,
    stepOut = handle_step_out,
    stackTrace = handle_stack_trace,
    scopes = handle_scopes,
    variables = handle_variables,
    disconnect = handle_disconnect,
    terminate = handle_disconnect,
}

local function dispatch(msg)
    if msg.type ~= "request" then return end
    local h = handlers[msg.command]
    if not h then
        send_response(msg, {}, false, "not supported: " .. tostring(msg.command))
        return
    end
    local ok, err = pcall(h, msg)
    if not ok then
        send_response(msg, {}, false, tostring(err))
    end
end

local function install_hook()
    -- Task 3 填充
end

function M.listen(host, port)
    host, port = env_host_port(host, port)
    state.host, state.port = host, port
    state.configured = false

    local server, err = socket.bind(host, port)
    if not server then error("bind failed " .. host .. ":" .. port .. " " .. tostring(err)) end
    state.server = server
    print(string.format("[lua-dap] listening on %s:%d, waiting for VS Code debugServer...", host, port))

    server:settimeout(nil)
    local client, aerr = server:accept()
    if not client then error("accept failed: " .. tostring(aerr)) end
    client:settimeout(nil)
    state.client = client
    print("[lua-dap] client connected")

    while not state.configured do
        local msg = read_message()
        dispatch(msg)
    end

    install_hook()
    return true
end

return M
```

注意：`json.encode` 的字符串长度按字节计；UTF-8 下 `#body` 与 Content-Length 一致（dkjson 输出 UTF-8）。

- [ ] **Step 5: 再跑握手测试**

```powershell
cd E:\demo\lua-dap-debugger\script\test
python test_dap_handshake.py
```

Expected: 打印 `handshake ok`

若 `require("socket")` 失败：确认 `bin/socket.dll`（或 Debug 输出目录）存在，并按测试脚本里的 `package.cpath` 补齐。

- [ ] **Step 6: Commit**

```powershell
git add script/lua-runtime/debugger.lua script/test/dap_client.py script/test/test_dap_handshake.py
git commit -m "feat: DAP Content-Length transport and attach handshake"
```

---

### Task 3: 断点命中 + hook 内同步暂停

**Files:**
- Modify: `script/lua-runtime/debugger.lua`
- Create: `script/test/run_debugee.lua`
- Create: `script/test/test_dap_breakpoint.py`

**Interfaces:**
- Consumes: Task 2 的 `listen` / `dispatch` / `normalize_path`
- Produces: `pause_loop(reason)`；命中断点发 `stopped`；`continue` 退出暂停

- [ ] **Step 1: 在 `debugger.lua` 实现 hook 与 pause_loop**

替换 `install_hook` 与相关 stub，加入：

```lua
local function current_depth()
    local d = 0
    while debug.getinfo(d + 1, "f") do
        d = d + 1
    end
    return d
end

local function is_debugger_file(source)
    if not source then return true end
    source = normalize_path(source:sub(1, 1) == "@" and source:sub(2) or source)
    return source:find("lua%-runtime/debugger%.lua", 1, false) ~= nil
        or source:find("lua%-runtime/dkjson%.lua", 1, false) ~= nil
end

local function pause_loop(reason, file, line)
    state.paused = true
    state.resume_cmd = nil
    state.var_refs = {}
    state.next_ref = 1000
    send_event("stopped", {
        reason = reason,
        threadId = 1,
        allThreadsStopped = true,
    })
    while state.paused do
        local msg = read_message()
        dispatch(msg)
    end
end

local function on_line()
    local info = debug.getinfo(2, "Sl")
    if not info or not info.source or info.source:sub(1, 1) ~= "@" then return end
    if is_debugger_file(info.source) then return end
    local file = normalize_path(info.source:sub(2))
    local line = info.currentline
    if line <= 0 then return end

    local file_bps = state.breakpoints[file]
    if file_bps and file_bps[line] then
        pause_loop("breakpoint", file, line)
        return
    end

    -- stepping filled in Task 5
end

function install_hook()
    debug.sethook(function(event)
        if event == "line" then
            on_line()
        end
    end, "l")
end
```

`handle_continue` 已在 Task 2 stub 中设置 `state.paused = false`。

- [ ] **Step 2: 写 debugee 入口**

创建 `script/test/run_debugee.lua`：

```lua
local root = arg[1] or "."
local port = tonumber(arg[2] or 18173)
package.path = root .. "/script/?.lua;" .. package.path
package.cpath = root .. "/bin/?.dll;" .. root .. "/bin/Debug/?.dll;" .. package.cpath

local dbg = require("lua-runtime.debugger")
dbg.listen("127.0.0.1", port)

local function work()
    local player = { name = "Ada", stats = { hp = 100, mp = 50 } }
    local x = 10
    local y = 20
    local sum = x + y  -- << breakpoint target: find this line number dynamically in test
    print("sum", sum, player.name, player.stats.hp)
end

work()
print("DEBUGEE_DONE")
```

- [ ] **Step 3: 写断点测试**

创建 `script/test/test_dap_breakpoint.py`：

```python
import re
import subprocess
import time
from pathlib import Path

from dap_client import DapClient

ROOT = Path(__file__).resolve().parents[2]
PORT = 18173
DEBUGEE = ROOT / "script" / "test" / "run_debugee.lua"


def find_lua():
    hits = list(ROOT.glob("**/lua.exe"))
    if not hits:
        raise SystemExit("lua.exe not found")
    return str(hits[0])


def breakpoint_line():
    text = DEBUGEE.read_text(encoding="utf-8")
    for i, line in enumerate(text.splitlines(), 1):
        if "local sum = x + y" in line:
            return i
    raise SystemExit("breakpoint line not found")


def main():
    lua = find_lua()
    line = breakpoint_line()
    src = str(DEBUGEE).replace("\\", "/")
    proc = subprocess.Popen(
        [lua, str(DEBUGEE), str(ROOT), str(PORT)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    time.sleep(0.6)
    c = DapClient("127.0.0.1", PORT, timeout=5.0)
    try:
        c.send_request("initialize", {"adapterID": "lua-dap"})
        c.wait_for(lambda m: m.get("type") == "response" and m.get("command") == "initialize")
        c.wait_for(lambda m: m.get("event") == "initialized")
        c.send_request("attach", {})
        c.wait_for(lambda m: m.get("command") == "attach" and m.get("type") == "response")
        c.send_request("setBreakpoints", {
            "source": {"path": src},
            "breakpoints": [{"line": line}],
        })
        c.wait_for(lambda m: m.get("command") == "setBreakpoints" and m.get("type") == "response")
        c.send_request("configurationDone", {})
        c.wait_for(lambda m: m.get("command") == "configurationDone" and m.get("type") == "response")

        stopped = c.wait_for(lambda m: m.get("event") == "stopped")
        assert stopped["body"]["reason"] == "breakpoint"

        c.send_request("continue", {"threadId": 1})
        c.wait_for(lambda m: m.get("command") == "continue" and m.get("type") == "response")
        out, _ = proc.communicate(timeout=5)
        assert "DEBUGEE_DONE" in out, out
        print("breakpoint ok")
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        c.close()


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: 跑测试**

```powershell
cd E:\demo\lua-dap-debugger\script\test
python test_dap_breakpoint.py
```

Expected: `breakpoint ok`

若路径匹配失败：在 `setBreakpoints` 与 hook 两侧打印 `normalize_path` 结果对比；确保 VS Code/测试传入的 path 与 `debug.getinfo` 的 `@` 路径规范化后一致。

- [ ] **Step 5: Commit**

```powershell
git add script/lua-runtime/debugger.lua script/test/run_debugee.lua script/test/test_dap_breakpoint.py
git commit -m "feat: line breakpoints with in-hook pause loop"
```

---

### Task 4: stackTrace / scopes / variables（含 table 展开）

**Files:**
- Modify: `script/lua-runtime/debugger.lua`
- Modify: `script/test/test_dap_breakpoint.py`（断言变量）

**Interfaces:**
- Produces:
  - `stackTrace` → `stackFrames[]`（`id` 从 0 起，对应 Lua 栈 level 偏移）
  - `scopes` → Locals / Upvalues，`variablesReference` 编码帧信息
  - `variables` → 标量与可展开 table

- [ ] **Step 1: 实现栈与变量收集**

在 `debugger.lua` 增加（可按需微调 ref 编码）：

```lua
-- ref 约定：
--   locals scope:  100000 + frameId
--   upvalues scope:200000 + frameId
--   table object:  300000 + running id  (state.var_refs[ref] = value)

local function lua_level_from_frame(frame_id)
    -- frame_id 0 = 最靠近暂停点的用户帧
    -- pause_loop 在 hook 内，debug.getinfo(2) 是用户行
    return frame_id + 3 -- 经验偏移：getinfo(1)=on_line相关, 实测时用测试校准
end
```

**校准要求：** 实现后用断点测试打印 `debug.getinfo(i,"Snl")` 若干层，选定稳定偏移，把 `lua_level_from_frame` 写死为正确值，并在注释标明。

`handle_stack_trace`：

```lua
local function handle_stack_trace(req)
    local frames = {}
    local i = 0
    while true do
        local level = lua_level_from_frame(i)
        local info = debug.getinfo(level, "Snl")
        if not info then break end
        if info.source and info.source:sub(1,1) == "@" and not is_debugger_file(info.source) then
            local path = normalize_path(info.source:sub(2))
            frames[#frames+1] = {
                id = i,
                name = info.name or "?",
                line = info.currentline or 0,
                column = 0,
                source = { path = path, name = path:match("([^/]+)$") },
            }
        end
        i = i + 1
        if i > 64 then break end
    end
    send_response(req, { stackFrames = frames, totalFrames = #frames })
end
```

`handle_scopes`：

```lua
local function handle_scopes(req)
    local frameId = (req.arguments or {}).frameId or 0
    send_response(req, {
        scopes = {
            { name = "Locals", variablesReference = 100000 + frameId, expensive = false },
            { name = "Upvalues", variablesReference = 200000 + frameId, expensive = false },
        }
    })
end
```

变量格式化与 table 展开：

```lua
local function alloc_ref(value)
    local ref = state.next_ref
    state.next_ref = state.next_ref + 1
    state.var_refs[ref] = value
    return ref
end

local function format_var(name, value)
    local t = type(value)
    if t == "table" then
        return {
            name = tostring(name),
            value = "table",
            type = "table",
            variablesReference = alloc_ref(value),
        }
    elseif t == "string" then
        return {
            name = tostring(name),
            value = string.format("%q", value),
            type = "string",
            variablesReference = 0,
        }
    else
        return {
            name = tostring(name),
            value = tostring(value),
            type = t,
            variablesReference = 0,
        }
    end
end

local function collect_locals(frameId)
    local level = lua_level_from_frame(frameId)
    local out = {}
    local i = 1
    while true do
        local name, value = debug.getlocal(level, i)
        if not name then break end
        if name:sub(1,1) ~= "(" then
            out[#out+1] = format_var(name, value)
        end
        i = i + 1
    end
    return out
end

local function collect_upvalues(frameId)
    local level = lua_level_from_frame(frameId)
    local info = debug.getinfo(level, "f")
    local out = {}
    if not info or not info.func then return out end
    local i = 1
    while true do
        local name, value = debug.getupvalue(info.func, i)
        if not name then break end
        out[#out+1] = format_var(name, value)
        i = i + 1
    end
    return out
end

local function collect_table(tbl)
    local out = {}
    for k, v in pairs(tbl) do
        out[#out+1] = format_var(k, v)
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

local function handle_variables(req)
    local ref = (req.arguments or {}).variablesReference or 0
    local vars
    if ref >= 200000 and ref < 300000 then
        vars = collect_upvalues(ref - 200000)
    elseif ref >= 100000 and ref < 200000 then
        vars = collect_locals(ref - 100000)
    else
        local tbl = state.var_refs[ref]
        vars = (type(tbl) == "table") and collect_table(tbl) or {}
    end
    send_response(req, { variables = vars })
end
```

把 Task 2 的 stub `handle_stack_trace` / `handle_scopes` / `handle_variables` 换成以上实现。

- [ ] **Step 2: 扩展断点测试断言变量**

在 `test_dap_breakpoint.py` 命中 `stopped` 之后、`continue` 之前插入：

```python
        c.send_request("stackTrace", {"threadId": 1})
        st = c.wait_for(lambda m: m.get("command") == "stackTrace" and m.get("type") == "response")
        frames = st["body"]["stackFrames"]
        assert frames, st
        frame_id = frames[0]["id"]

        c.send_request("scopes", {"frameId": frame_id})
        sc = c.wait_for(lambda m: m.get("command") == "scopes" and m.get("type") == "response")
        locals_ref = sc["body"]["scopes"][0]["variablesReference"]

        c.send_request("variables", {"variablesReference": locals_ref})
        vr = c.wait_for(lambda m: m.get("command") == "variables" and m.get("type") == "response")
        names = {v["name"]: v for v in vr["body"]["variables"]}
        assert "x" in names and "y" in names and "player" in names, names
        assert names["player"]["variablesReference"] > 0
        c.send_request("variables", {"variablesReference": names["player"]["variablesReference"]})
        pr = c.wait_for(lambda m: m.get("command") == "variables" and m.get("type") == "response")
        pnames = {v["name"]: v for v in pr["body"]["variables"]}
        assert "name" in pnames and "stats" in pnames, pnames
```

- [ ] **Step 3: 跑测试并校准栈偏移**

```powershell
python test_dap_breakpoint.py
```

Expected: `breakpoint ok`，且变量断言通过。若 locals 为空，按 Step 1 注释校准 `lua_level_from_frame`。

- [ ] **Step 4: Commit**

```powershell
git add script/lua-runtime/debugger.lua script/test/test_dap_breakpoint.py
git commit -m "feat: DAP stackTrace, scopes, and table variables"
```

---

### Task 5: 步进（next / stepIn / stepOut）

**Files:**
- Modify: `script/lua-runtime/debugger.lua`
- Modify: `script/test/test_dap_breakpoint.py` 或新建 `script/test/test_dap_step.py`（可选，至少手工步骤写清）

**Interfaces:**
- Consumes: `pause_loop`、`current_depth`、`on_line`
- Produces: `next`/`stepIn`/`stepOut` 按 spec 深度规则停

- [ ] **Step 1: 完善 continue/step handlers 与 on_line**

```lua
local function handle_continue(req)
    state.step = nil
    state.paused = false
    state.resume_cmd = "continue"
    send_response(req, { allThreadsContinued = true })
end

local function handle_next(req)
    state.step = "over"
    state.step_depth = current_depth()
    state.paused = false
    send_response(req, {})
end

local function handle_step_in(req)
    state.step = "in"
    state.step_depth = current_depth()
    state.paused = false
    send_response(req, {})
end

local function handle_step_out(req)
    state.step = "out"
    state.step_depth = current_depth()
    state.paused = false
    send_response(req, {})
end
```

在 `on_line` 断点检查之后追加：

```lua
    if state.step == "in" then
        state.step = nil
        pause_loop("step", file, line)
        return
    elseif state.step == "over" then
        local d = current_depth()
        if d <= state.step_depth then
            state.step = nil
            pause_loop("step", file, line)
        end
        return
    elseif state.step == "out" then
        local d = current_depth()
        if d < state.step_depth then
            state.step = nil
            pause_loop("step", file, line)
        end
        return
    end
```

注意：`current_depth()` 在 hook 与 handler 中调用时深度基准可能差一层；以实测为准，必要时在 handler 里用 `current_depth() - 1` 记录。

- [ ] **Step 2: 手工或脚本验证步进**

最少验证：

1. 断点停住 → Step Over → `stopped.reason == step` 且行号前进
2. Step Into 进入函数
3. Step Out 回到调用者

可把断言加进 `test_dap_breakpoint.py`，或临时用 VS Code（Task 6 之后）验证。

- [ ] **Step 3: Commit**

```powershell
git add script/lua-runtime/debugger.lua script/test/*.py
git commit -m "feat: DAP step in/over/out"
```

---

### Task 6: 宿主接入 + sample + launch.json + README

**Files:**
- Modify: `main/main.cpp`
- Modify: `script/sample/main.lua`
- Modify: `.vscode/launch.json`
- Modify: `README.md`

**Interfaces:**
- Consumes: `require("lua-runtime.debugger").listen`
- Produces: 可运行的 `main.exe` + VS Code F5 流程

- [ ] **Step 1: 改 `main/main.cpp`**

在 `RunFile(sample)` 之前插入：

```cpp
    try {
        lua.safe_script(R"(
local host = os.getenv("LUADAP_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("LUADAP_PORT") or "8172")
local dbg = require("lua-runtime.debugger")
dbg.listen(host, port)
)");
    } catch (const std::exception& e) {
        std::cerr << "Debugger listen failed: " << e.what() << std::endl;
        return 1;
    }
```

确保 `AddScriptPath` / `AddLibraryPath` 已指向 `script` 与 `bin`（保持现有逻辑；如路径写死，可暂保留 V1 demo 硬编码）。

- [ ] **Step 2: 更新 sample**

改写 `script/sample/main.lua`：

```lua
print("Lua DAP Debugger sample start")

local function add(a, b)
    return a + b
end

local function main()
    local player = {
        name = "Ada",
        stats = { hp = 100, mp = 30 },
    }
    local x = 10
    local y = 20
    local sum = add(x, y) -- 在此行打断点，检查 player/x/y/sum
    print("sum =", sum)
    print("player", player.name, player.stats.hp)
    print("done")
end

main()
```

- [ ] **Step 3: 更新 `.vscode/launch.json`**

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Lua DAP Attach (debugServer)",
      "type": "node",
      "request": "attach",
      "debugServer": 8172
    }
  ]
}
```

- [ ] **Step 4: 重写 README 核心用法（中文）**

至少包含：

1. 编译宿主，确保 `bin/socket.dll` 与 `bin/main.exe` 可用
2. 先启动 `main.exe`，看到 `listening on 127.0.0.1:8172`
3. VS Code 打开仓库，F5 选 `Lua DAP Attach (debugServer)`
4. 在 `script/sample/main.lua` 打断点，查看 Variables

注明 `vscode-extension/` 为 V1 非必需。

- [ ] **Step 5: 编译并冒烟**

```powershell
# 按仓库现有 CMake/MSVC 流程编译
# 然后：
.\bin\main.exe
# 另开 VS Code F5 attach
```

Expected: 满足 spec 验收标准 1–6。

- [ ] **Step 6: Commit**

```powershell
git add main/main.cpp script/sample/main.lua .vscode/launch.json README.md
git commit -m "feat: wire host listen and VS Code debugServer workflow"
```

---

### Task 7: Spec 状态回写 + 最终自检

**Files:**
- Modify: `docs/superpowers/specs/2026-08-15-lua-dap-debugger-design.md`（状态改为已实现/已批准）

- [ ] **Step 1: 跑全部自动化冒烟**

```powershell
cd E:\demo\lua-dap-debugger\script\test
python test_dap_handshake.py
python test_dap_breakpoint.py
```

Expected: 两个都 ok。

- [ ] **Step 2: 对照 spec 验收清单勾选**

1. main 监听并等待  
2. debugServer 握手后脚本跑  
3. 断点行正确  
4. locals + table 成员可见  
5. continue / step 可用  
6. 停止调试不卡死  

- [ ] **Step 3: 更新 spec 状态行并提交**

把文首状态改为：`已批准并完成 V1 实现`（若实现中有小偏差，在 spec 末尾加「实现备注」一节，写清实际栈偏移等）。

```powershell
git add docs/superpowers/specs/2026-08-15-lua-dap-debugger-design.md
git commit -m "docs: mark Lua DAP V1 design implemented"
```

---

## Plan Self-Review

| Spec 要求 | 对应 Task |
|-----------|-----------|
| 标准 DAP + luasocket | Task 2 |
| debugServer 直连，无扩展桥 | Task 6 |
| listen 阻塞到 configurationDone | Task 2 |
| 断点 | Task 3 |
| locals + table 成员 | Task 4 |
| continue / next / stepIn / stepOut | Task 5 |
| threads / setExceptionBreakpoints | Task 2 |
| dkjson | Task 1 |
| sample + README + launch.json | Task 6 |
| 同机路径规范化 | Task 2/3 `normalize_path` |
| hook 内同步暂停 | Task 3 |

无 TBD/TODO 占位；接口名在任务间一致（`M.listen`、`normalize_path`、`pause_loop`）。
