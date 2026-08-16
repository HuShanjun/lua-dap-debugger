-- Multi-coroutine sample for luadap (DAP threads = Lua coroutines).
-- Host: dap.start(..., false) then loops update() + dap.update().
--
-- How to try in VS Code:
-- 1. Launch the C++ host (listen on 8172).
-- 2. Attach with debugServer: 8172.
-- 3. Set breakpoints on the lines marked BREAKPOINT below (inside workers).
-- 4. Call Stack / Threads: main + coro-2 / coro-3 (names may vary by id).

print("sample: multi-coroutine start _VERSION =", _VERSION)

require "sample.functions"


local function worker_alpha(name)
    local tick = 0
    while true do
        tick = tick + 1
        local n = tick * 2 -- BREAKPOINT: alpha
        print("[alpha]", name, "tick", tick, "n", n)
        coroutine.yield()
    end
end

local function worker_beta(tag)
    local shared = { tag = tag }
    local bag = {
        name = "beta",
        stats = { hp = 100, mp = 30 },
        shared_a = shared,
        shared_b = shared,
    }
    bag.self = bag -- circular; expand carefully in Variables
    local step = 0
    while true do
        step = step + 1
        local sum = step + bag.stats.hp -- BREAKPOINT: beta
        print("[beta]", bag.name, "step", step, "sum", sum)
        coroutine.yield()
    end
end

-- Wrapped by luadap after start: each create is tracked as a DAP thread.
local cos = {
    coroutine.create(function()
        worker_alpha("A")
    end),
    coroutine.create(function()
        worker_beta("shared")
    end),
}

-- Optional explicit name (same registry path as create wrap):
-- local dap = require("luadap")
-- dap.track(cos[1], "alpha")
-- dap.track(cos[2], "beta")

local rr = 0

function update(count)
    if #cos == 0 then
        return
    end
    functions.add(1, 2) 
    rr = (rr % #cos) + 1
    local co = cos[rr]
    local st = coroutine.status(co)
    if st == "dead" then
        print("coro dead, index", rr)
        return
    end
    local ok, err = coroutine.resume(co)
    if not ok then
        error("resume failed: " .. tostring(err))
    end
    if count % 20 == 0 then
        print("update count =", count, "last coro", rr, "status", coroutine.status(co))
    end
end
