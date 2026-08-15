local root = arg[1] or "."
local port = tonumber(arg[2] or 18174)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, true)

local function inner()
    local z = 42
    return z
end

local function work()
    local a = 1
    local b = inner()
    local c = b + 1
    print("STEP_DONE", a, b, c)
end

work()
print("DEBUGEE_DONE")
io.stdout:flush()
