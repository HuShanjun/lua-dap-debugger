-- Debugee that uses ONLY bin/luadap.dll (embedded debugger/dkjson/asyncsocket).
-- package.path is empty so disk lua-runtime cannot be required (proves embed).
-- Usage: lua run_debugee_luadap.lua <repo_root> <port>
local root = arg[1] or "."
local port = tonumber(arg[2] or 18180)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, true)
print("LISTEN_DONE")
io.stdout:flush()

local function work()
    local player = { name = "Ada", stats = { hp = 100, mp = 50 } }
    local x = 10
    local y = 20
    local sum = x + y
    print("sum", sum, player.name, player.stats.hp)
end

work()
print("DEBUGEE_DONE")
io.stdout:flush()
