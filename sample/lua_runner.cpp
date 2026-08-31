#include "lua_runner.h"
#include <filesystem>
#include <format>

auto launch_dbg = R"(
    local dap = require("luadap")
    dap.start("127.0.0.1", 8172, false)
)";
    
auto update_dbg = R"(
    local dap = require("luadap")
    dap.update()
)";

LuaRunner::LuaRunner()
{

}

LuaRunner::~LuaRunner()
{

}

void LuaRunner::Init()
{
    lua_.open_libraries();
    lua_.safe_script(launch_dbg);
    AddScriptPath("E:/demo/lua-dap-debugger/sample/script");
    AddLibraryPath("E:/demo/lua-dap-debugger/bin");
    
    RunFile("E:/demo/lua-dap-debugger/sample/script/main.lua");

    GetLuaFunction("update", main_update_);
}

void LuaRunner::Shutdown()
{
}

void LuaRunner::update()
{
    lua_.safe_script(update_dbg);
    if (main_update_.valid()) {
        main_update_();
    }
}

void LuaRunner::GetLuaFunction(const std::string& name, sol::function& func)
{
    func = lua_[name];
    if (!func.valid()) {
        throw std::runtime_error(std::format("Lua function {} not found", name));
    }
}

void LuaRunner::RunFile(const std::string& file)
{
    auto result = lua_.safe_script_file(file);
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error(std::format("Lua file {} error: {}", file, err.what()));
    }
}

void LuaRunner::AddScriptPath(const std::string& path)
{
    // path格式：E:/demo/lua-dap-debugger/script/?.lua
    if (path.empty()) {
        return;
    }
    std::filesystem::path path_obj(path);
    auto lua_path = (path_obj / "?.lua").generic_string();
    const std::string& old_path = lua_["package"]["path"];
    lua_["package"]["path"] = old_path + ";" + lua_path;
}

void LuaRunner::AddLibraryPath(const std::string& path)
{
    if (path.empty()) {
        return;
    }
    std::filesystem::path path_obj(path);
#if defined(WIN32)
    auto lua_path = (path_obj / "?.dll").generic_string();
#else
    auto lua_path = (path_obj / "?.so").generic_string();
#endif
    const std::string& old_path = lua_["package"]["cpath"];
    lua_["package"]["cpath"] = old_path + ";" + lua_path;
}