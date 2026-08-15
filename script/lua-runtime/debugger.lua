local asyncsocket = require("asyncsocket")
local json = require("lua-runtime.dkjson")
-- luasocket is optional: used only for a short sleep so listen/pause pumps
-- do not spin the CPU. All DAP I/O goes through asyncsocket.
local socket_ok, socket = pcall(require, "socket")

local M = {}

local state = {
    host = "127.0.0.1",
    port = 8172,
    sock = nil,
    recv_buf = "",
    reader_coro = nil,
    client_open = false,
    seq = 0,
    configured = false,
    breakpoints = {}, -- [norm_path] = { [line] = { condition = string|nil } }
    var_refs = {},
    table_to_ref = {}, -- table identity -> variablesReference (same stop)
    next_ref = 1000,
    step = nil, -- nil | "in" | "over" | "out"
    step_depth = 0,
    paused = false,
    pause_thread = nil, -- main thread object while inside pause_loop
    resume_cmd = nil,
    dead = false,
    close_pending = false,
}

local function short_sleep()
    if socket_ok and socket.sleep then
        socket.sleep(0.001)
    end
end

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
    if not state.sock then error("send failed: no socket") end
    state.seq = state.seq + 1
    obj.seq = state.seq
    local body = json.encode(obj)
    local frame = string.format("Content-Length: %d\r\n\r\n%s", #body, body)
    state.sock:send(frame)
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

-- Parse one complete DAP frame from state.recv_buf. Incomplete header/body
-- returns nil without consuming bytes. A full header with no Content-Length
-- or invalid JSON is a fatal error (caller / reader coro handles teardown).
local function try_parse_one_dap_frame()
    local buf = state.recv_buf or ""
    local sep = buf:find("\r\n\r\n", 1, true)
    if not sep then return nil end
    local header = buf:sub(1, sep - 1)
    local len = header:match("[Cc]ontent%-[Ll]ength:%s*(%d+)")
    if not len then error("missing Content-Length") end
    len = tonumber(len)
    local body_start = sep + 4
    if #buf < body_start + len - 1 then return nil end
    local body = buf:sub(body_start, body_start + len - 1)
    state.recv_buf = buf:sub(body_start + len)
    local obj, _, jerr = json.decode(body)
    if not obj then error("json decode: " .. tostring(jerr)) end
    return obj
end

local function handle_initialize(req)
    send_response(req, {
        supportsConfigurationDoneRequest = true,
        supportsSetVariable = false,
        supportsConditionalBreakpoints = true,
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
        local cond = bp.condition
        if type(cond) == "string" then
            cond = cond:match("^%s*(.-)%s*$")
            if cond == "" then cond = nil end
        else
            cond = nil
        end
        state.breakpoints[path][line] = { condition = cond }
        local verified = { line = line, verified = true }
        if cond then verified.condition = cond end
        out[#out + 1] = verified
    end
    send_response(req, { breakpoints = out })
end

local function handle_configuration_done(req)
    send_response(req, {})
    state.configured = true
end

-- Tear down the DAP session: optional disconnect reply, terminated event,
-- unload hook, close asyncsocket, clear debug state. Idempotent.
local function shutdown_session(req)
    if state.dead then
        state.paused = false
        state.configured = true
        return
    end
    state.dead = true
    state.close_pending = false
    state.paused = false
    state.configured = true
    state.step = nil
    state.step_depth = 0
    state.resume_cmd = "disconnect"

    local sock = state.sock
    local can_send = sock and state.client_open
    if can_send then
        if req then
            pcall(send_response, req, {})
        end
        pcall(send_event, "terminated", {})
    end

    debug.sethook()

    state.sock = nil
    state.client_open = false
    state.reader_coro = nil
    state.recv_buf = ""
    state.pause_thread = nil
    if sock then
        pcall(function() sock:close() end)
    end
    state.breakpoints = {}
    state.var_refs = {}
    state.table_to_ref = {}
    state.next_ref = 1000
end

local function handle_disconnect(req)
    shutdown_session(req)
end

local function is_debugger_file(source)
    if not source then return true end
    source = normalize_path(source:sub(1, 1) == "@" and source:sub(2) or source)
    return source:find("lua%-runtime/debugger%.lua", 1, false) ~= nil
        or source:find("lua%-runtime/dkjson%.lua", 1, false) ~= nil
end

-- Dispatch runs in the DAP reader coroutine, so debug.getinfo/getlocal on
-- the current stack cannot see the paused debugee. While pause_loop is on
-- the main thread, walk that thread instead.
local function paused_other_thread()
    local th = state.pause_thread
    return th and th ~= coroutine.running() and th or nil
end

local function info_at(level, what)
    local th = paused_other_thread()
    if th then return debug.getinfo(th, level, what) end
    return debug.getinfo(level, what)
end

local function local_at(level, i)
    local th = paused_other_thread()
    if th then return debug.getlocal(th, level, i) end
    return debug.getlocal(level, i)
end

-- Count user frames only so pause handlers and on_line share a baseline.
-- Raw getinfo depth is larger inside pause_loop (pcall / dispatch / handler).
local function current_depth()
    local d = 0
    local level = 1
    while true do
        local info = info_at(level, "S")
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

-- DAP reader coroutine: yield on incomplete frames; dispatch complete ones.
-- on_message only appends recv_buf; M.update resumes this until it yields.
local function reader_main()
    while not state.dead do
        local msg = try_parse_one_dap_frame()
        if not msg then
            coroutine.yield()
        else
            dispatch(msg)
        end
    end
end

function M.update()
    asyncsocket.pump()
    if not state.dead then
        local co = state.reader_coro
        if co and coroutine.status(co) ~= "dead" then
            for _ = 1, 32 do
                local ok, err = coroutine.resume(co)
                if not ok then
                    shutdown_session()
                    error(err)
                end
                if state.dead then break end
                if coroutine.status(co) == "suspended" then
                    break
                end
                if coroutine.status(co) == "dead" then break end
            end
        end
    end
    -- CLOSE may share a pump batch with MESSAGE (e.g. disconnect + TCP FIN).
    -- Drain the reader first so the last DAP frame is dispatched; then teardown.
    if state.close_pending then
        state.close_pending = false
        if not state.dead then
            shutdown_session()
        end
    end
end

-- ref 约定：
--   locals scope:  100000 + frameId
--   upvalues scope:200000 + frameId
--   table object:  state.next_ref (reset to 1000 each stop; stays < 100000)
--   Same Lua table object reuses one ref (table_to_ref). Circular children
--   along the expansion ancestor chain are shown as non-expandable.
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
    local cross = paused_other_thread() ~= nil
    -- Skip this helper only when it sits on the stack being walked.
    local level = cross and 1 or 2
    while #frames < 64 do
        local info = info_at(level, "Snlf")
        if not info then break end
        if info.source and info.source:sub(1, 1) == "@" and not is_debugger_file(info.source) then
            -- Same-thread: store level-1 so getlocal is relative to the caller.
            -- Cross-thread: store the real level on the pause thread.
            frames[#frames + 1] = { level = cross and level or (level - 1), info = info }
        end
        level = level + 1
    end
    return frames
end

local function alloc_ref(value)
    local existing = state.table_to_ref[value]
    if existing then
        return existing
    end
    local ref = state.next_ref
    state.next_ref = state.next_ref + 1
    state.var_refs[ref] = value
    state.table_to_ref[value] = ref
    return ref
end

-- ancestors: set of tables on the path from the expanded root to the parent
local function format_var(name, value, ancestors)
    local t = type(value)
    if t == "table" then
        if ancestors and ancestors[value] then
            return {
                name = tostring(name),
                value = "table (circular)",
                type = "table",
                variablesReference = 0,
            }
        end
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
        local name, value = local_at(level, i)
        if not name then break end
        if name:sub(1, 1) ~= "(" then
            out[#out + 1] = format_var(name, value, nil)
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
        out[#out + 1] = format_var(name, value, nil)
        i = i + 1
    end
    return out
end

local function collect_table(tbl, ancestors)
    local next_anc = {}
    if ancestors then
        for k, v in pairs(ancestors) do
            next_anc[k] = v
        end
    end
    next_anc[tbl] = true
    local out = {}
    for k, v in pairs(tbl) do
        out[#out + 1] = format_var(k, v, next_anc)
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
        if type(tbl) == "table" then
            -- Seed ancestors with this table so self-refs / back-edges are
            -- marked circular even on the first expand request.
            vars = collect_table(tbl, { [tbl] = true })
        else
            vars = {}
        end
    end
    send_response(req, { variables = vars })
end

handlers.stackTrace = handle_stack_trace
handlers.scopes = handle_scopes
handlers.variables = handle_variables

local function pause_loop(reason, file, line)
    state.paused = true
    state.pause_thread = coroutine.running()
    state.resume_cmd = nil
    state.var_refs = {}
    state.table_to_ref = {}
    state.next_ref = 1000
    local sent = pcall(send_event, "stopped", {
        reason = reason,
        threadId = 1,
        allThreadsStopped = true,
    })
    if not sent then
        shutdown_session()
        return
    end
    while state.paused do
        local ok = pcall(M.update)
        if not ok then
            -- Client dropped / parse error: unload hook, leave pause.
            shutdown_session()
            break
        end
        short_sleep()
    end
    state.pause_thread = nil
end

local function eval_breakpoint_condition(condition, level_from_caller)
    if not condition or condition == "" then
        return true
    end
    -- level_from_caller is relative to on_line; this frame sits in between.
    local level = level_from_caller + 1
    local env = {}
    local i = 1
    while true do
        local name, value = debug.getlocal(level, i)
        if not name then break end
        if name:sub(1, 1) ~= "(" then
            env[name] = value
        end
        i = i + 1
    end
    local info = debug.getinfo(level, "f")
    if info and info.func then
        local j = 1
        while true do
            local name, value = debug.getupvalue(info.func, j)
            if not name then break end
            if env[name] == nil then
                env[name] = value
            end
            j = j + 1
        end
    end
    setmetatable(env, { __index = _G })
    local chunk = load("return (" .. condition .. ")", "@bp_condition", "t", env)
    if not chunk then
        return false
    end
    local ok, res = pcall(chunk)
    if not ok then
        return false
    end
    return not not res
end

local function on_line()
    if state.dead or not state.client_open then return end
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
    local bp = file_bps and file_bps[line]
    if bp then
        if eval_breakpoint_condition(bp.condition, level) then
            pause_loop("breakpoint", file, line)
            return
        end
        -- Condition false: do not stop; still allow stepping below.
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
    state.dead = false
    state.close_pending = false
    state.paused = false
    state.client_open = false
    state.recv_buf = ""
    state.seq = 0

    state.sock = asyncsocket.listen(host, port)
    state.reader_coro = coroutine.create(reader_main)
    state.sock:on_open(function()
        state.client_open = true
        print("[lua-dap] client connected")
    end)
    -- Spec: on_message ONLY appends recv_buf; M.update resumes the reader.
    state.sock:on_message(function(chunk)
        state.recv_buf = state.recv_buf .. (chunk or "")
    end)
    -- Spec: on_close only flags; M.update drains the reader, then shuts down.
    -- Immediate shutdown here would drop a MESSAGE+CLOSE batch (no terminated).
    state.sock:on_close(function()
        if not state.dead then
            state.close_pending = true
        end
    end)
    print(string.format("[lua-dap] listening on %s:%d, waiting for VS Code debugServer...", host, port))

    while not state.configured do
        local ok = pcall(M.update)
        if not ok then
            shutdown_session()
            break
        end
        short_sleep()
    end

    -- Disconnect during handshake must not install a hook on a dead session.
    if state.client_open and not state.dead then
        install_hook()
    end
    return true
end

-- Host should call this after the debugee script returns so a still-connected
-- client gets a single `terminated` event and sockets/hook are released.
function M.shutdown()
    shutdown_session()
end

return M
