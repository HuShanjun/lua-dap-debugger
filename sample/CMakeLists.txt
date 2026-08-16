cmake_minimum_required(VERSION 3.16)

project(sample LANGUAGES C CXX)


add_executable(sample main.cpp lua_runner.cpp)
target_include_directories(sample PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(sample PRIVATE liblua)
