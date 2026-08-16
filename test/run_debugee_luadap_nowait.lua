-- nowait debugee: start(false) must return before DAP handshake.
-- Handshake is driven by a later update() loop (host game-loop style).
-- package.path is empty so disk lua-runtime cannot be required (proves embed).
-- Usage: lua run_debugee_luadap_nowait.lua <repo_root> <port>
local root = arg[1] or "."
local port = tonumber(arg[2] or 18181)
root = root:gsub("\\", "/")

package.path = ""
package.cpath = root .. "/bin/?.dll"

local dap = require("luadap")
dap.start("127.0.0.1", port, false)
print("START_RETURNED")
io.stdout:flush()

-- wait=false installs the line hook from update() after configurationDone.
local deadline = os.clock() + 8
while not debug.gethook() do
    if os.clock() > deadline then
        error("nowait: handshake did not complete (no hook after update loop)")
    end
    dap.update()
end
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
