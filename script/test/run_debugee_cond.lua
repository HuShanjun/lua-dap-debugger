local root = arg[1] or "."
local port = tonumber(arg[2] or 18175)
package.path = root .. "/script/?.lua;" .. package.path
package.cpath = root .. "/bin/?.dll;" .. root .. "/bin/Debug/?.dll;" .. package.cpath

local dbg = require("lua-runtime.debugger")
dbg.listen("127.0.0.1", port)

local function work()
    local hits = 0
    for i = 1, 5 do
        hits = hits + 1 -- breakpoint target: condition i == 3
    end
    print("hits", hits)
end

work()
print("DEBUGEE_DONE")
dbg.shutdown()
