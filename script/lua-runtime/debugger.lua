--[[
    debugger.lua — 通用 Lua 调试运行时
    纯 Lua 实现，基于 debug 库 + debug.sethook
    通过 TCP socket 与 VS Code DAP 前端通信（自定义 JSON 协议）
    也可作为命令行调试器独立运行（scratchpad 模式）

    通信协议（与前端 DataProcessor 对齐）：
    消息 = JSON字符串 + 分隔符 "\n"
    前端→后端命令：{"cmd":"continue","callbackId":1,"info":{}}
    后端→前端事件：{"event":"stopped","reason":"breakpoint","file":"...","line":10}
]]--

local socket = require("socket")
local ltn12 = require("ltn12")

-- ===================== 配置 =====================
local PORT = tonumber(os.getenv("LUADAP_PORT") or "8172")
local HOST = os.getenv("LUADAP_HOST") or "127.0.0.1"

-- ===================== 状态 =====================
local state = {
    breakpoints = {},      -- key: normalized_path:line -> {condition, hitCount, ignoreCount}
    running = false,       -- 是否在暂停等待前端命令
    step_request = nil,    -- "into" | "over" | "out" | nil
    step_depth = 0,        -- 进入/退出时记录栈深度
    server = nil,          -- TCP server
    client = nil,          -- TCP client
    coro_debugger = nil,
    coro_debugee = nil,
    hook_installed = false,
    base_dir = "",
    pending_responses = {}, -- callbackId -> 用于等待前端确认的请求
    outputs = {},
    logging = false,
}

-- ===================== 工具函数 =====================
local function log(...) if state.logging then print("[DEBUGGER]", ...) end end

-- 规范化路径（把 cwd 前缀去掉，方便断点匹配）
local function normalize_path(path)
    if not path then return path end
    local cwd = lfs and lfs.currentdir() or ""
    if cwd and path:sub(1, #cwd) == cwd then
        path = path:sub(#cwd + 1)
        if path:sub(1,1) == "." then path = path:sub(2) end
        if path:sub(1,1) == "/" then path = path:sub(2) end
    end
    return path
end

-- 取当前执行文件的规范路径
local function current_source()
    local info = debug.getinfo(2, "S")
    if info and info.source then
        local src = info.source
        if src:sub(1,1) == "@" then return normalize_path(src:sub(2)) end
    end
    return "(unknown)"
end

-- ===================== 调试钩子 =====================
-- 钩子掩码 "lcr" = line + call + return，覆盖三种事件
local HOOK_MASK = "lcr"

local function install_hook()
    if state.hook_installed then return end
    debug.sethook(function() debugger_hook() end, HOOK_MASK, 100) -- count=100 减少开销
    state.hook_installed = true
    log("hook installed")
end

local function remove_hook()
    if not state.hook_installed then return end
    debug.sethook()
    state.hook_installed = false
    log("hook removed")
end

function debugger_hook()
    local event = debug.gethookinfo and debug.gethookinfo() or nil
    -- 通过 debug.getinfo 拿当前行信息
    local info = debug.getinfo(0, "lS")
    local line = info and info.currentline or 0
    local source = info and info.source
    local file = "(unknown)"
    if source and source:sub(1,1) == "@" then
        file = normalize_path(source:sub(2))
    end

    log("hook:", (debug.getinfo(0,"n") or {}).name or "?", "line=", line, "event mask=", debug.gethook() and "lcr" or "?")

    -- 1) 检查断点
    local bp_key = file .. ":" .. line
    local bp = state.breakpoints[bp_key]
    if bp then
        -- 条件断点
        if bp.condition then
            local ok, res = pcall(function()
                return load("return " .. bp.condition, "@condition")() == true
            end)
            if not ok or not res then return end
        end
        -- 命中计数
        if bp.ignoreCount and bp.ignoreCount > 0 then
            bp.ignoreCount = bp.ignoreCount - 1
            return
        end
        bp.hitCount = (bp.hitCount or 0) + 1
        -- 命中断点 → 暂停
        pause("breakpoint", file, line, bp_key)
        return
    end

    -- 2) 步进处理
    if state.step_request then
        local ev_type = nil
        -- 通过 getinfo 的 "t" 或判断调用深度变化来区分 call/return/line
        -- 简单方案：每次 line 事件都可能是 step 的目标
        local req = state.step_request
        state.step_request = nil

        if req == "into" then
            -- Step Into：每次行事件都停
            pause("step", file, line)
            return
        elseif req == "over" then
            -- Step Over：停在新行，但跳过函数内部
            -- 通过记录进入/退出深度判断
            local depth = get_stack_depth()
            if depth > state.step_depth then
                -- 进入了新函数，继续直到返回到原深度
                state.step_request = "over_wait"
                return
            end
            pause("step", file, line)
            return
        elseif req == "out" then
            local depth = get_stack_depth()
            if depth < state.step_depth then
                -- 已经返回到调用者了
                pause("step", file, line)
                return
            end
            -- 还在函数内，等返回
            state.step_request = "out_wait"
            return
        end
    end

    -- "over_wait"：等回到原深度
    if state.step_request == "over_wait" then
        local depth = get_stack_depth()
        if depth <= state.step_depth then
            state.step_request = nil
            pause("step", file, line)
        end
        return
    end

    -- "out_wait"：等更深一层返回
    if state.step_request == "out_wait" then
        local depth = get_stack_depth()
        if depth < state.step_depth then
            state.step_request = nil
            pause("step", file, line)
        end
        return
    end
end

-- 获取当前调用栈深度
local function get_stack_depth()
    local depth = 0
    while debug.getinfo(depth, "t") do depth = depth + 1 end
    return depth
end

-- ===================== 暂停 / 恢复 =====================
function pause(reason, file, line, bp_key)
    state.running = false
    -- 发送 stopped 事件
    send_event({
        event = "stopped",
        reason = reason,
        file = file,
        line = line,
        bp_key = bp_key,
    })
    -- 等待前端命令（继续/步进/断开）
    -- 前端会通过 send_command 回复 continue/step 等
    log("paused:", reason, file, line)
end

-- 恢复执行
function resume()
    state.running = true
    state.step_request = nil
    log("resumed")
end

-- ===================== 命令处理 =====================
-- 前端发来的命令格式：{"cmd":"...","callbackId":N,"info":{...}}
local function handle_command(msg)
    local cmd = msg.cmd
    local cb = msg.callbackId
    local info = msg.info or {}

    log("recv cmd:", cmd, "cb=", cb)

    if cmd == "setBreakpoint" then
        local src = info.source and info.source.path
        local lines = info.breakpoints or {}
        for _, bp in ipairs(lines) do
            local key = normalize_path(src) .. ":" .. bp.line
            state.breakpoints[key] = {
                line = bp.line,
                condition = bp.condition,
                hitCount = 0,
                ignoreCount = bp.ignoreConditions and bp.ignoreCount or nil,
            }
        end
        send_response(cb, { breakpoints = {} }) -- 实际应返回验证后的断点
        return

    elseif cmd == "continue" then
        resume()
        send_response(cb, {})

    elseif cmd == "step" then
        local grp = info.granularity
        if grp == "instruction" then
            resume() -- 简单处理，按行步进
        end
        state.step_request = "into"
        state.step_depth = get_stack_depth()
        send_response(cb, {})

    elseif cmd == "stepIn" then
        state.step_request = "into"
        state.step_depth = get_stack_depth()
        send_response(cb, {})

    elseif cmd == "stepOver" then
        state.step_request = "over"
        state.step_depth = get_stack_depth()
        send_response(cb, {})

    elseif cmd == "stepOut" then
        state.step_request = "out"
        state.step_depth = get_stack_depth()
        send_response(cb, {})

    elseif cmd == "setVariable" or cmd == "setExpression" then
        -- 简单不支持，回复 ok
        send_response(cb, { value = "(not supported)" })

    elseif cmd == "evaluate" then
        local expr = info.expression
        local ok, val = pcall(function()
            local chunk = load("return " .. expr, "@eval")
            return chunk()
        end)
        if ok then
            send_response(cb, { result = tostring(val), type = typeof(val) })
        else
            send_response(cb, { result = "error: " .. tostring(val), type = "error" })
        end

    elseif cmd == "stack" then
        local frames = collect_stack()
        send_response(cb, { stack = frames })

    elseif cmd == "scopes" then
        local frames = collect_stack()
        local scopes = {}
        if frames and frames[1] then
            scopes[1] = { name = "Locals", variablesReference = 1000 + (info.frameId or 0) }
        end
        send_response(cb, { scopes = scopes })

    elseif cmd == "variables" then
        local ref = info.variablesReference
        local vars = collect_variables(ref)
        send_response(cb, { variables= vars })

    elseif cmd == "disconnect" or cmd == "exit" then
        remove_hook()
        if state.client then state.client:close() end
        if state.server then state.server:close() end
        -- 退出进程
        os.exit(0)

    elseif cmd == "done" then
        remove_hook()
        if state.client then state.client:close() end
        if state.server then state.server:close() end
    end
end

-- ===================== 信息收集 =====================
function collect_stack()
    local stack = {}
    local depth = 0
    while true do
        local info = debug.getinfo(depth + 1, "nSlupf")
        if not info then break end
        depth = depth + 1
        local entry = {
            id = depth,
            name = info.name or "(anonymous)",
            file = info.source and info.source:match("@(.+)$") or "(unknown)",
            line = info.currentline or 0,
        }
        if entry.file then entry.file = normalize_path(entry.file) end
        stack[depth] = entry
    end
    return stack
end

function collect_variables(ref)
    -- ref 编码：高 12 位是 frameId，低位是类型（1=local, 2=upvalue）
    local frame_id = math.floor(ref / 1000)
    local var_type = ref % 1000

    local vars = {}

    if var_type == 1 then
        -- 局部变量：通过 debug.getlocal(frame_id, i)
        local i = 1
        while true do
            local ok, name, value = pcall(debug.getlocal, frame_id, i)
            if not ok or not name then break end
            if name ~= "(temporary)" then
                vars[i] = { name = name, value = format_value(value), type = typeof(value) }
            end
            i = i + 1
        end
    elseif var_type == 2 then
        -- upvalue：通过 debug.getupvalue(function_index, i)
        -- 简化：扫描当前帧函数的 upvalue
        local func = debug.getfunc(frame_id)
        if func then
            local i = 1
            while true do
                local ok, name, value = pcall(debug.getupvalue, func, i)
                if not ok or not name then break end
                vars[i] = { name = name, value = format_value(value), type = typeof(value) }
                i = i + 1
            end
        end
    end

    return vars
end

function format_value(v)
    local t = typeof(v)
    if t == "string" then
        return "\"" .. v .. "\""
    elseif t == "table" then
        return "{table: 0x" .. tostring(v):match("0x.%s*$") .. "}"
    end
    return tostring(v)
end

function typeof(v)
    local t = type(v)
    if t == "number" then return "number" end
    if t == "string" then return "string" end
    if t == "boolean" then return "boolean" end
    if t == "table" then return "table" end
    if t == "function" then return "function" end
    return t
end

-- ===================== 网络通信 =====================
local SEP = "\n"

function send_message(obj)
    local json = require("cjson") or require("dkjson")
    local str = json.encode(obj) .. SEP
    if state.client then
        state.client:send(str)
    elseif state.server and state.server ~= true then
        -- server mode: 广播给所有连接
        local clients = state._clients or {}
        for _, c in ipairs(clients) do c:send(str) end
    end
end

function send_event(ev)
    send_message({ event = ev.event, reason = ev.reason, file = ev.file, line = ev.line, seq = ev.seq })
end

function send_response(cb, body)
    send_message({ type = "response", callbackId = cb, body = body })
end

-- 读循环：解析前端发来的命令
local function read_loop(client)
    local buf = ""
    while true do
        local chunk, err = client:receive(1) -- 每次读 1 字节，简单解析
        if not chunk then
            if err == "closed" then break end
            socket.sleep(0.01)
            goto continue
        end
        buf = buf .. chunk
        local idx = buf:find(SEP)
        if idx then
            local line = buf:sub(1, idx - 1)
            buf = buf:sub(idx + 1)
            local ok, msg = pcall(function()
                local cjson = require("cjson") or require("dkjson")
                return cjson.decode(line)
            end)
            if ok and msg then
                handle_command(msg)
            end
        end
        ::continue::
    end
end

-- ===================== 服务器模式（前端作为 client 连进来） =====================
function start_server(host, port)
    state.server = true
    state._clients = {}
    local server = socket.bind(host, port)
    if not server then
        error("无法绑定端口 " .. port .. "：可能已被占用")
    end
    server:settimeout(0.1)
    state.server = server
    print(string.format("[debugger] 监听 %s:%d，等待前端连接...", host, port))

    while true do
        local client = server:accept()
        if client then
            client:settimeout(0.1)
            table.insert(state._clients, client)
            -- 新连接：通知前端已就绪
            send_message({ event = "initialized" })
            -- 起一个协程读这个 client
            coroutine.wrap(function() read_loop(client) end)()
        else
            socket.sleep(0.05)
        end
    end
end

-- ===================== 客户端模式（连前端） =====================
function start_client(host, port)
    local c = socket.connect(host, port)
    if not c then
        error("无法连接 " .. host .. ":" .. port)
    end
    c:settimeout(0.1)
    state.client = c
    -- 通知前端
    send_message({ event = "initialized" })
    coroutine.wrap(function() read_loop(c) end)()
end

-- ===================== CLI 模式（无前端，手动调试） =====================
local function start_cli()
    print("Lua CLI Debugger — 输入 ? 查看命令")
    install_hook()
    local commands = {
        ["c"] = "继续 (continue)",
        ["s"] = "单步进入 (step in)",
        ["n"] = "单步跳过 (step over)",
        ["f"] = "单步出 (step out)",
        ["b"] = "断点: b file:line [cond]",
        ["bl"] = "列出断点",
        ["p"] = "打印: p var",
        ["bt"] = "调用栈",
        ["?"] = "帮助",
        ["q"] = "退出",
    }
    while true do
        io.write("lua-debug> ")
        local line = io.read()
        if not line then break end
        local cmd, arg = line:match("^(%S+)%s*(.-)%s*$")
        cmd = cmd or ""
        if cmd == "c" then
            resume()
        elseif cmd == "s" then
            state.step_request = "into"
            state.step_depth = get_stack_depth()
        elseif cmd == "n" then
            state.step_request = "over"
            state.step_depth = get_stack_depth()
        elseif cmd == "f" then
            state.step_request = "out"
            state.step_depth = get_stack_depth()
        elseif cmd == "b" then
            local spec, cond = arg:match("(.+)$(.*)$")
            local file, lnum = arg:match("([%w%p]+):(%d+)")
            if file and lnum then
                state.breakpoints[normalize_path(file) .. ":" .. lnum] = {
                    line = tonumber(lnum), condition = cond or nil, hitCount = 0
                }
                print("断点已设置:", file, lnum)
            else
                print("用法: b file:line [条件]")
            end
        elseif cmd == "bl" then
            for k, v in pairs(state.breakpoints) do
                print(string.format("  %s  hits=%d cond=%s", k, v.hitCount, v.condition or "-"))
            end
        elseif cmd == "p" then
            local ok, val = pcall(function()
                return load("return " .. arg)()
            end)
            if ok then print(format_value(val)) else print("error:", val) end
        elseif cmd == "bt" then
            local stack = collect_stack()
            for i = #stack, 1, -1 do
                local f = stack[i]
                print(string.format("  #%d  %s at %s:%d", i, f.name, f.file, f.line))
            end
        elseif cmd == "?" then
            for k, v in pairs(commands) do print(string.format("  %-6s %s", k, v)) end
        elseif cmd == "q" then
            break
        end
    end
    remove_hook()
end

-- ===================== 入口 =====================
local function main()
    -- 参数：lua debugger.lua [--server|--client] [host] [port] [-- run program]
    local args = {...}
    local mode = "client" -- 默认客户端模式（连 VS Code）

    local i = 1
    local program_args = {}
    while i <= #args do
        local a = args[i]
        if a == "--server" then
            mode = "server"
            i = i + 1
            if args[i] then HOST = args[i]; i = i + 1 end
            if args[i] then PORT = tonumber(args[i]); i = i + 1 end
        elseif a == "--client" then
            mode = "client"
            i = i + 1
            if args[i] then HOST = args[i]; i = i + 1 end
            if args[i] then PORT = tonumber(args[i]); i = i + 1 end
        elseif a == "--" then
            i = i + 1
            while i <= #args do program_args[#program_args+1] = args[i]; i = i + 1 end
        elseif a == "--cli" then
            mode = "cli"
            i = i + 1
        else
            program_args[#program_args+1] = a
            i = i + 1
        end
    end

    if mode == "cli" then
        start_cli()
        return
    end

    -- server / client / launch 模式
    if mode == "server" then
        start_server(HOST, PORT)
        return
    end

    -- client 模式：连接前端（VS Code）
    -- 但如果没连上，退化为 CLI
    local ok, err = pcall(start_client, HOST, PORT)
    if not ok then
        print(string.format("[debugger] %s，进入 CLI 模式", err))
        start_cli()
        return
    end

    -- 如果是 launch 模式（有 program 参数），加载并运行目标脚本
    -- 这个逻辑由外层 wrapper 脚本处理，这里只做调试器本身
end

main()
