-- Multi-coro threads smoke: after start(wait=true), wrappers register
-- coroutine.create so DAP threads lists main + the live coroutine.
-- Usage: lua run_debugee_coro_threads.lua <repo_root> <port>
local root = arg[1] or "."
local port = tonumber(arg[2] or 18200)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, true)

-- wrappers already active after start:
local co = coroutine.create(function()
    return 1
end)
assert(co)
assert(dap.track) -- will fail until exported

print("CORO_READY")
io.stdout:flush()

while true do
    dap.update()
end
