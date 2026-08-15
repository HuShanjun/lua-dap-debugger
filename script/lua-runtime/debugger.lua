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

local function is_debugger_file(source)
    if not source then return true end
    source = normalize_path(source:sub(1, 1) == "@" and source:sub(2) or source)
    return source:find("lua%-runtime/debugger%.lua", 1, false) ~= nil
        or source:find("lua%-runtime/dkjson%.lua", 1, false) ~= nil
end

-- Count user frames only so pause handlers and on_line share a baseline.
-- Raw getinfo depth is larger inside pause_loop (pcall / dispatch / handler).
local function current_depth()
    local d = 0
    local level = 1
    while true do
        local info = debug.getinfo(level, "S")
        if not info then break end
        if info.source and info.source:sub(1, 1) == "@" and not is_debugger_file(info.source) then
            d = d + 1
        end
        level = level + 1
    end
    return d
end

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
-- Forward-declared: real bodies sit after walk_user_frames.
local handle_stack_trace, handle_scopes, handle_variables

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

-- ref 约定：
--   locals scope:  100000 + frameId
--   upvalues scope:200000 + frameId
--   table object:  state.next_ref (reset to 1000 each stop; stays < 100000)
--
-- Stack calibration (Task 3 + Task 4):
-- Do NOT use debug.getinfo(2) or a blind frameId+3 offset. The set-hook
-- wrapper plus pause_loop / dispatch / pcall sit above the debugee, and
-- the number of debugger frames differs between stackTrace and getlocal.
-- Reuse on_line's walk: skip debugger.lua / dkjson.lua and non-@ sources.
-- walk_user_frames stores level-1 so getlocal/getinfo levels are relative
-- to the *caller* of the helper (collect_locals vs handle_stack_trace).
-- frame_id 0 = closest user frame to the pause point (work() in the test).

local function walk_user_frames()
    local frames = {}
    local level = 2 -- skip this helper
    while #frames < 64 do
        local info = debug.getinfo(level, "Snlf")
        if not info then break end
        if info.source and info.source:sub(1, 1) == "@" and not is_debugger_file(info.source) then
            frames[#frames + 1] = { level = level - 1, info = info }
        end
        level = level + 1
    end
    return frames
end

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
    -- Must call walk_user_frames from here (not via another helper) so the
    -- stored level-1 is relative to this function, matching getlocal(level).
    local frames = walk_user_frames()
    local f = frames[(frameId or 0) + 1]
    local level = f and f.level
    local out = {}
    if not level then return out end
    local i = 1
    while true do
        local name, value = debug.getlocal(level, i)
        if not name then break end
        if name:sub(1, 1) ~= "(" then
            out[#out + 1] = format_var(name, value)
        end
        i = i + 1
    end
    return out
end

local function collect_upvalues(frameId)
    local frames = walk_user_frames()
    local f = frames[(frameId or 0) + 1]
    local out = {}
    if not f or not f.info.func then return out end
    local i = 1
    while true do
        local name, value = debug.getupvalue(f.info.func, i)
        if not name then break end
        out[#out + 1] = format_var(name, value)
        i = i + 1
    end
    return out
end

local function collect_table(tbl)
    local out = {}
    for k, v in pairs(tbl) do
        out[#out + 1] = format_var(k, v)
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

handle_stack_trace = function(req)
    local frames = {}
    for i, f in ipairs(walk_user_frames()) do
        local info = f.info
        local path = normalize_path(info.source:sub(2))
        frames[#frames + 1] = {
            id = i - 1,
            name = info.name or "?",
            line = info.currentline or 0,
            column = 0,
            source = { path = path, name = path:match("([^/]+)$") },
        }
    end
    send_response(req, { stackFrames = frames, totalFrames = #frames })
end

handle_scopes = function(req)
    local frameId = (req.arguments or {}).frameId or 0
    send_response(req, {
        scopes = {
            { name = "Locals", variablesReference = 100000 + frameId, expensive = false },
            { name = "Upvalues", variablesReference = 200000 + frameId, expensive = false },
        }
    })
end

handle_variables = function(req)
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

handlers.stackTrace = handle_stack_trace
handlers.scopes = handle_scopes
handlers.variables = handle_variables

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
    -- Wrapper hook + debugger internals sit above the debugee; walk to the
    -- first non-debugger @source so getinfo(2) is not the set-hook closure.
    local info
    local level = 2
    while true do
        info = debug.getinfo(level, "Sl")
        if not info then return end
        if info.source and info.source:sub(1, 1) == "@" and not is_debugger_file(info.source) then
            break
        end
        level = level + 1
    end
    local file = normalize_path(info.source:sub(2))
    local line = info.currentline
    if line <= 0 then return end

    local file_bps = state.breakpoints[file]
    if file_bps and file_bps[line] then
        pause_loop("breakpoint", file, line)
        return
    end

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
end

local function install_hook()
    debug.sethook(function(event)
        if event == "line" then
            on_line()
        end
    end, "l")
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
