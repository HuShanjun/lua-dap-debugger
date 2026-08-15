local root = arg[1] or "."
local port = tonumber(arg[2] or 18174)
package.path = root .. "/script/?.lua;" .. package.path
package.cpath = root .. "/bin/?.dll;" .. root .. "/bin/Debug/?.dll;" .. package.cpath

local dbg = require("lua-runtime.debugger")
dbg.listen("127.0.0.1", port)

local function work()
    local shared = { tag = "shared" }
    local node = { name = "root", shared_a = shared, shared_b = shared }
    node.self = node -- circular
    local x = 1
    local stop = node.name -- breakpoint here
    print(stop, node.shared_a.tag)
end

work()
print("DEBUGEE_DONE")
dbg.shutdown()
