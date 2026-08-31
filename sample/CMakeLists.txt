cmake_minimum_required(VERSION 3.16)

project(sample LANGUAGES C CXX)


add_executable(mult_thread mult_thread.cpp lua_runner.cpp)
target_include_directories(mult_thread PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(mult_thread PRIVATE liblua)

add_executable(single_thread single_thread.cpp lua_runner.cpp)
target_include_directories(single_thread PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(single_thread PRIVATE liblua)

add_executable(mult_vm mult_vm.cpp lua_runner.cpp)
target_include_directories(mult_vm PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(mult_vm PRIVATE liblua)