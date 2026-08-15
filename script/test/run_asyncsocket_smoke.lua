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
local s = asyncsocket.listen(host, port)

local saw_close = false
s:on_open(function()
    print("OPEN")
    io.stdout:flush()
end)
s:on_message(function(chunk)
    print("MSG " .. chunk)
    io.stdout:flush()
end)
s:on_close(function()
    print("CLOSE")
    io.stdout:flush()
    saw_close = true
end)

print("LISTENING")
io.stdout:flush()

local ok_sleep, socket = pcall(require, "socket")
local pumps = 0
while not saw_close and pumps < 2000 do
    asyncsocket.pump()
    pumps = pumps + 1
    if ok_sleep then
        socket.sleep(0.01)
    end
end

if not saw_close then
    io.stderr:write("timeout waiting for CLOSE\n")
    os.exit(1)
end
