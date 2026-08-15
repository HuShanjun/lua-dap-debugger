print("Lua DAP Debugger sample start")

local function add(a, b)
    return a + b
end

local function main()
    local player = {
        name = "Ada",
        stats = { hp = 100, mp = 30 },
    }
    local x = 10
    local y = 20
    local sum = add(x, y) -- 在此行打断点，检查 player/x/y/sum
    print("sum =", sum)
    print("player", player.name, player.stats.hp)
    print("done")
end

main()
