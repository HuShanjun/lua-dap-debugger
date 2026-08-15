cmake_minimum_required(VERSION 3.16)

project(main LANGUAGES C CXX)


add_executable(main main.cpp)
target_link_libraries(main PRIVATE liblua)