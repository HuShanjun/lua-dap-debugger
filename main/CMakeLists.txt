cmake_minimum_required(VERSION 3.16)

project(main LANGUAGES C CXX)


add_executable(main main.cpp)
target_include_directories(main PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(main PRIVATE liblua)
