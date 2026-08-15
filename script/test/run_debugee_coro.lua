-- Multi-coro breakpoint: coroutine.create worker hits a line BP.
-- Usage: lua run_debugee_coro.lua <repo_root> <port>
local root = arg[1] or "."
local port = tonumber(arg[2] or 18201)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, true)

local function worker()
  local a = 10
  local b = 20
  local sum = a + b  -- breakpoint target
  return sum
end

local co = coroutine.create(worker)
coroutine.resume(co)
print("DEBUGEE_DONE")
io.stdout:flush()
