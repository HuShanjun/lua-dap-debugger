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
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&]() {
            LuaRunner lua_runner;
            lua_runner.Init();
            while (true) {
                lua_runner.update();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            lua_runner.Shutdown();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::cout << "All threads joined" << std::endl;
    return 0;
}
