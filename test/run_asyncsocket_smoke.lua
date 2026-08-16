-- Smoke helper for test_asyncsocket_smoke.py
-- Usage: lua run_asyncsocket_smoke.lua <repo_root> <host> <port>
-- Protocol tokens (Python): LISTENING, LISTENING2, OPEN, CONCAT hello, CLOSE,
-- RELISTEN, OPEN2, CLOSE2, JOINED
local root = arg[1]
local host = arg[2] or "127.0.0.1"
local port = tonumber(arg[3]) or 18221
if not root then
    io.stderr:write("usage: run_asyncsocket_smoke.lua <repo_root> <host> <port>\n")
    os.exit(2)
end

root = root:gsub("\\", "/")
package.path = root .. "/sample/script/?.lua;" .. package.path
package.cpath = root .. "/bin/?.dll;" .. root .. "/bin/Debug/?.dll;" .. package.cpath

local asyncsocket = require("asyncsocket")
local ok_sleep, socket = pcall(require, "socket")

local function sleep(sec)
    if ok_sleep then
        socket.sleep(sec)
        return
    end
    if asyncsocket.sleep then
        asyncsocket.sleep(sec)
        return
    end
    local until_t = os.clock() + sec
    while os.clock() < until_t do
    end
end

local function pump_until(pred, max_pumps)
    local pumps = 0
    while not pred() and pumps < max_pumps do
        asyncsocket.pump()
        pumps = pumps + 1
        sleep(0.01)
    end
    return pred()
end

local srv = asyncsocket.listen(host, port)

local received = ""
local saw_close = false
srv:on_accept(function(conn)
    print("OPEN")
    io.stdout:flush()
    conn:on_message(function(chunk)
        print("MSG " .. chunk)
        io.stdout:flush()
        received = received .. chunk
        if received == "hello" then
            print("CONCAT hello")
            io.stdout:flush()
            conn:send(received)
        end
    end)
    conn:on_close(function()
        print("CLOSE")
        io.stdout:flush()
        saw_close = true
    end)
end)

print("LISTENING")
io.stdout:flush()

if not pump_until(function()
    return saw_close
end, 2000) then
    io.stderr:write("timeout waiting for CLOSE\n")
    os.exit(1)
end

if received ~= "hello" then
    io.stderr:write("expected concatenated hello, got: " .. received .. "\n")
    os.exit(1)
end

-- Second listen must work after :close() without waiting for GC.
-- srv:close() stops listen only; the inbound conn already closed.
srv:close()
local srv2, err = asyncsocket.listen(host, port)
if not srv2 then
    io.stderr:write("second listen failed: " .. tostring(err) .. "\n")
    os.exit(1)
end
print("RELISTEN")
io.stdout:flush()

-- Explicit conn:close() while a client is still connected: on_close via pump.
local opened2 = false
local saw_close2 = false
local conn2 = nil
srv2:on_accept(function(conn)
    print("OPEN2")
    io.stdout:flush()
    opened2 = true
    conn2 = conn
    conn:on_close(function()
        print("CLOSE2")
        io.stdout:flush()
        saw_close2 = true
    end)
end)

print("LISTENING2")
io.stdout:flush()

if not pump_until(function()
    return opened2
end, 2000) then
    io.stderr:write("timeout waiting for OPEN2\n")
    os.exit(1)
end

conn2:close()
if not pump_until(function()
    return saw_close2
end, 2000) then
    io.stderr:write("explicit close did not deliver on_close\n")
    os.exit(1)
end
srv2:close()
print("JOINED")
io.stdout:flush()
