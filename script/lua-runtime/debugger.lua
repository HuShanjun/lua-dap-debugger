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
