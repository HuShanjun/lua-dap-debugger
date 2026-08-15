-- Long-lived nowait debugee for reconnect tests: start(false) + forever update().
-- Usage: lua run_debugee_luadap_reconnect.lua <repo_root> <port>
local root = arg[1] or "."
local port = tonumber(arg[2] or 18210)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, false)
print("START_RETURNED")
io.stdout:flush()

local n = 0
while true do
    dap.update()
    n = n + 1
    if n == 1 then
        print("UPDATE_LOOP")
        io.stdout:flush()
    end
end
