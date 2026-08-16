local root = arg[1] or "."
local port = tonumber(arg[2] or 18173)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, true)

local function work()
    local player = { name = "Ada", stats = { hp = 100, mp = 50 } }
    local x = 10
    local y = 20
    local sum = x + y  -- << breakpoint target: find this line number dynamically in test
    print("sum", sum, player.name, player.stats.hp)
end

work()
print("DEBUGEE_DONE")
io.stdout:flush()
