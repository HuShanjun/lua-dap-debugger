print("Lua DAP Debugger sample start")

local function add(a, b)
    return a + b
end

local function main()
    local shared = { tag = "shared" }
    local player = {
        name = "Ada",
        stats = { hp = 100, mp = 30 },
        shared_a = shared,
        shared_b = shared,
    }
    player.self = player -- 循环引用：展开时应显示 table (circular)
    local x = 10
    local y = 20
    local sum = add(x, y) -- 在此行打断点，展开 player
    print("sum =", sum)
    print("player", player.name, player.stats.hp)
    print("done")
end

main()

function update(count)
    print("update count = " .. count)
end
