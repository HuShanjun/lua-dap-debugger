-- Smoke helper for test_asyncsocket_smoke.py
-- Usage: lua run_asyncsocket_smoke.lua <repo_root> <host> <port>
local root = arg[1]
local host = arg[2] or "127.0.0.1"
local port = tonumber(arg[3]) or 18221
if not root then
    io.stderr:write("usage: run_asyncsocket_smoke.lua <repo_root> <host> <port>\n")
    os.exit(2)
end

root = root:gsub("\\", "/")
package.path = root .. "/script/?.lua;" .. package.path
package.cpath = root .. "/bin/?.dll;" .. root .. "/bin/Debug/?.dll;" .. package.cpath

local asyncsocket = require("asyncsocket")
local ok_sleep, socket = pcall(require, "socket")

local function sleep(sec)
    if ok_sleep then
        socket.sleep(sec)
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

local s = asyncsocket.listen(host, port)

local received = ""
local saw_close = false
s:on_open(function()
    print("OPEN")
    io.stdout:flush()
end)
s:on_message(function(chunk)
    print("MSG " .. chunk)
    io.stdout:flush()
    received = received .. chunk
    if received == "hello" then
        print("CONCAT hello")
        io.stdout:flush()
        s:send(received)
    end
end)
s:on_close(function()
    print("CLOSE")
    io.stdout:flush()
    saw_close = true
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
s:close()
local s2, err = asyncsocket.listen(host, port)
if not s2 then
    io.stderr:write("second listen failed: " .. tostring(err) .. "\n")
    os.exit(1)
end
print("RELISTEN")
io.stdout:flush()

-- Explicit :close() while a client is still connected: join + sync on_close.
local opened2 = false
local saw_close2 = false
s2:on_open(function()
    print("OPEN2")
    io.stdout:flush()
    opened2 = true
end)
s2:on_close(function()
    print("CLOSE2")
    io.stdout:flush()
    saw_close2 = true
end)

print("LISTENING2")
io.stdout:flush()

if not pump_until(function()
    return opened2
end, 2000) then
    io.stderr:write("timeout waiting for OPEN2\n")
    os.exit(1)
end

s2:close()
if not saw_close2 then
    io.stderr:write("explicit close did not deliver on_close\n")
    os.exit(1)
end
print("JOINED")
io.stdout:flush()
