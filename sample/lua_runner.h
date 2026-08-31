#pragma once
#include <sol/sol.hpp>

class LuaRunner
{
    LuaRunner(const LuaRunner&) = delete;
    LuaRunner& operator=(const LuaRunner&) = delete;
    
public:
    LuaRunner(LuaRunner&&) = default;
    LuaRunner& operator=(LuaRunner&&) = default;
    LuaRunner();
    ~LuaRunner();

    void Init();
    void Shutdown();
    void update();

private:
    void RunFile(const std::string& file);
    void AddScriptPath(const std::string& path);
    void AddLibraryPath(const std::string& path);
    void GetLuaFunction(const std::string& name, sol::function& func);

private:
    sol::state lua_;
    sol::function main_update_;
};