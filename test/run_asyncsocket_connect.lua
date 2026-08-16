-- Connect helper for test_asyncsocket_connect.py
-- Usage: lua run_asyncsocket_connect.lua <repo_root> <host> <port>
-- Same process: listen + connect; exchange ping/pong via callbacks + pump.
-- Protocol tokens (Python): LISTENING, ACCEPT, OPEN, PING, PONG, DONE
local root = arg[1]
local host = arg[2] or "127.0.0.1"
local port = tonumber(arg[3]) or 18223
if not root then
    io.stderr:write("usage: run_asyncsocket_connect.lua <repo_root> <host> <port>\n")
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

local got_pong = false
local srv_conn = nil

local srv = asyncsocket.listen(host, port)
srv:on_accept(function(conn)
    print("ACCEPT")
    io.stdout:flush()
    srv_conn = conn
    conn:on_message(function(chunk)
        if chunk == "ping" then
            print("PING")
            io.stdout:flush()
            conn:send("pong")
        end
    end)
end)

print("LISTENING")
io.stdout:flush()

local client = asyncsocket.connect(host, port)
client:on_open(function()
    print("OPEN")
    io.stdout:flush()
    client:send("ping")
end)
client:on_message(function(chunk)
    if chunk == "pong" then
        print("PONG")
        io.stdout:flush()
        got_pong = true
    end
end)

if not pump_until(function()
    return got_pong
end, 2000) then
    io.stderr:write("timeout waiting for ping/pong\n")
    os.exit(1)
end

client:close()
if srv_conn then
    srv_conn:close()
end
srv:close()
print("DONE")
io.stdout:flush()
