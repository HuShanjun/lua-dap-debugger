-- logic state for same-thread multi-state DAP tests.
-- Host sets package.cpath to <repo>/bin/?.dll (Lua ABI of this build).
package.path = ""
if type(BIN) == "string" and BIN ~= "" then
    package.cpath = BIN .. "/?.dll" -- bin/?.dll
end

local dap = require("luadap")
dap.start("127.0.0.1", PORT, false, "logic")

function tick()
    local x = 1
    return x
end
