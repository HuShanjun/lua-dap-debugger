-- Multi-client helper for test_asyncsocket_multi.py
-- Usage: lua run_asyncsocket_multi.lua <repo_root> <host> <port>
-- Protocol tokens (Python): LISTENING, ACCEPT, ECHO, CLOSE, DONE
local root = arg[1]
local host = arg[2] or "127.0.0.1"
local port = tonumber(arg[3]) or 18222
if not root then
    io.stderr:write("usage: run_asyncsocket_multi.lua <repo_root> <host> <port>\n")
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

local accepted = 0
local echoed = 0
local closed = 0

local srv = asyncsocket.listen(host, port)
srv:on_accept(function(conn)
    accepted = accepted + 1
    print("ACCEPT " .. accepted)
    io.stdout:flush()
    conn:on_message(function(chunk)
        conn:send(chunk)
        echoed = echoed + 1
        print("ECHO " .. chunk)
        io.stdout:flush()
    end)
    conn:on_close(function()
        closed = closed + 1
        print("CLOSE " .. closed)
        io.stdout:flush()
    end)
end)

print("LISTENING")
io.stdout:flush()

if not pump_until(function()
    return accepted >= 2 and echoed >= 2 and closed >= 2
end, 2000) then
    io.stderr:write(string.format(
        "timeout waiting for 2-client echo; accepted=%d echoed=%d closed=%d\n",
        accepted, echoed, closed
    ))
    os.exit(1)
end

srv:close()
print("DONE")
io.stdout:flush()
