local root = arg[1] or "."
local port = tonumber(arg[2] or 18175)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, true)

local function work()
    local hits = 0
    for i = 1, 5 do
        hits = hits + 1 -- breakpoint target: condition i == 3
    end
    print("hits", hits)
end

work()
print("DEBUGEE_DONE")
io.stdout:flush()
