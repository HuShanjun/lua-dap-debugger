local root = arg[1] or "."
local port = tonumber(arg[2] or 18174)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, true)

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
io.stdout:flush()
