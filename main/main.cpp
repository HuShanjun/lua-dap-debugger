#include <iostream>
#include <thread>
#include <chrono>
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

auto launch_dbg = R"(
local dap = require("luadap")
local host = os.getenv("LUADAP_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("LUADAP_PORT") or "8172")
dap.start(host, port, true)
)";

int main(int argc, char* args[])
{
    sol::state lua;
    lua.open_libraries();
    AddScriptPath(lua, "E:/demo/lua-dap-debugger/script");
    AddLibraryPath(lua, "E:/demo/lua-dap-debugger/bin");
    std::cout << "hello world" << std::endl;

    sol::protected_function dbg_update;
    try {
        auto result = lua.safe_script(launch_dbg);
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "Debugger listen failed: " << err.what() << std::endl;
            return 1;
        }
        sol::protected_function require_fn = lua["require"];
        sol::table dbg_mod = require_fn("luadap");
        dbg_update = dbg_mod["update"];
    } catch (const std::exception& e) {
        std::cerr << "Debugger listen failed: " << e.what() << std::endl;
        return 1;
    }

    RunFile(lua, "E:/demo/lua-dap-debugger/script/sample/main.lua");

    // Pump asyncsocket DAP every tick. If the script defines update(), call it too.
    int update_count = 0;
    sol::protected_function lua_main_update = lua["update"];
    for (;;) {
        if (lua_main_update.valid()) {
            auto biz = lua_main_update(++update_count);
            if (!biz.valid()) {
                sol::error err = biz;
                std::cerr << "Script update: " << err.what() << std::endl;
                break;
            }
        }
        auto result = dbg_update();
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "Debugger update: " << err.what() << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return 0;
}
