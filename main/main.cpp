#include <iostream>
#include <sol/sol.hpp>
#include <filesystem>

void AddScriptPath(sol::state& lua, const std::string& path)
{
    // path格式：E:/demo/lua-dap-debugger/script/?.lua
    if (path.empty()) {
        return;
    }
    std::filesystem::path path_obj(path);
    auto lua_path = (path_obj / "?.lua").generic_string();
    const std::string& old_path = lua["package"]["path"];
    lua["package"]["path"] = old_path + ";" + lua_path;
}

void AddLibraryPath(sol::state& lua, const std::string& path)
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
    const std::string& old_path = lua["package"]["cpath"];
    lua["package"]["cpath"] = old_path + ";" + lua_path;
}

bool RunFile(sol::state& lua, const std::string& file)
{
    if (file.empty()) {
        return false;
    }
    try {
        auto result = lua.safe_script_file(file);
        if (result.valid()) {
            return true;
        }
        sol::error err = result;
        std::cerr << "Error: " << err.what() << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return false;
    }
}

int main(int argc, char* args[])
{
    sol::state lua;
    lua.open_libraries();
    AddScriptPath(lua, "E:/demo/lua-dap-debugger/script");
    AddLibraryPath(lua, "E:/demo/lua-dap-debugger/bin");
    std::cout << "hello world" << std::endl;
    try {
        auto result = lua.safe_script(R"(
local host = os.getenv("LUADAP_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("LUADAP_PORT") or "8172")
local dbg = require("lua-runtime.debugger")
dbg.listen(host, port)
)");
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "Debugger listen failed: " << err.what() << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Debugger listen failed: " << e.what() << std::endl;
        return 1;
    }
    RunFile(lua, "E:/demo/lua-dap-debugger/script/sample/main.lua");
    auto shutdown = lua.safe_script("require('lua-runtime.debugger').shutdown()");
    if (!shutdown.valid()) {
        sol::error err = shutdown;
        std::cerr << "Debugger shutdown: " << err.what() << std::endl;
    }
    return 0;
}
