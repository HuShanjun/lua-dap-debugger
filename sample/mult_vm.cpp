#include <iostream>
#include <thread>
#include <chrono>
#include <sol/sol.hpp>
#include <filesystem>
#include <vector>

#include "lua_runner.h"

int main(int argc, char* args[])
{
    (void)argc;
    (void)args;
    std::vector<LuaRunner> lua_runners;
    for (int i = 0; i < 10; i++) {
        lua_runners.emplace_back();
        lua_runners[i].Init();
    }
    while (true) {
        for (auto& lua_runner : lua_runners) {
            lua_runner.update();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (auto& lua_runner : lua_runners) {
        lua_runner.Shutdown();
    }
    std::cout << "All threads joined" << std::endl;
    return 0;
}
