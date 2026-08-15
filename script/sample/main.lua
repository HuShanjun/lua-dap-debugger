--[[ sample/main.lua — 调试器使用示例 ]]--
print("Lua DAP Debugger 示例程序启动")

local function add(a, b)
    return a + b
end

local function fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end

local function main()
    local x = 10
    local y = 20
    local sum = add(x, y)          -- 第 24 行：在这里打断点试试
    print("sum =", sum)

    for i = 1, 10 do
        local val = fib(i)
        print(i, "=>", val)
        if i == 5 then
            local tmp = val * 2
            print("tmp =", tmp)
        end
    end

    print("完成！")
end

main()
