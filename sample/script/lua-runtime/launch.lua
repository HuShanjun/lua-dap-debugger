--[[
    launch.lua — DAP launch 模式 wrapper
    用法：lua launch.lua <debugger.lua路径> [host] [port] -- <program> [args...]
    先加载调试器，安装钩子，再 dofile 目标程序
]]--

local debugger_path = arg[1]
local host = arg[2] or "127.0.0.1"
local port = arg[3] or "8172"
-- 找到 "--" 分隔符
local prog_arg_start = nil
for i = 4, #arg do
    if arg[i] == "--" then prog_arg_start = i + 1; break end
    if prog_arg_start == nil then prog_arg_start = i end
end

-- 设置环境变量，debugger.lua 会读这些
os.setenv("LUADAP_HOST", host)
os.setenv("LUADAP_PORT", port)

-- 加载调试器（会安装 hook）
local ok, err = pcall(function()
    require(debugger_path:match("^(.-\\.lua)$") and debugger_path or debugger_path)
end)
if not ok then
    -- 尝试 dofile
    dofile(debugger_path)
end

-- 运行目标程序
if prog_arg_start then
    local prog = arg[prog_arg_start]
    local pargs = {}
    for i = prog_arg_start + 1, #arg do pargs[#pargs+1] = arg[i] end
    -- 设置 cwd 和 arg
    local function run(p, a)
        local function wrapper()
            local function set_args(a)
                for i = 1, #a do arg[i] = a[i] end
                arg[0] = p
            end
            set_args(a)
            local chunk, load_err = loadfile(p)
            if not chunk then error(load_err) end
            chunk()
        end
        return wrapper
    end
    -- 简单做法：直接在正确路径加载
    local chunk, load_err = loadfile(prog)
    if not chunk then
        print("错误: 无法加载 " .. prog .. ": " .. tostring(load_err))
        os.exit(1)
    end
    -- 把程序参数放到 arg 里
    for i = 1, #pargs do arg[i] = pargs[i] end
    arg[0] = prog
    chunk()
end
