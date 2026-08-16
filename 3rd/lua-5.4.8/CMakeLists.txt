# 要求cmake是3.x的版本
cmake_minimum_required(VERSION 3.20)

# 解决方案名字叫lua-5.4.6
project(lua-5.4.6)

# 使用的语言标准是C89/ANSI C
set(CMAKE_C_STANDARD 90)

# 添加附加目录
include_directories(src)
include_directories(inc)
add_compile_options(-fPIC)
file(GLOB LUALIB_SRC inc/*.h src/*.c)

list(REMOVE_ITEM LUALIB_SRC 
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lua.c  
    ${CMAKE_CURRENT_SOURCE_DIR}/src/luac.c
)

# 创建子项目——liblua.lib
# 生成动态库——liblua.lib   XX为版本号，这里为54
add_library(liblua ${LUALIB_SRC})
set_property(TARGET liblua PROPERTY FOLDER "lua")

# 创建子项目——lua
# 生成lua解释器——lua.exe
add_executable(lua ${LUALIB_SRC} src/lua.c)
set_property(TARGET lua PROPERTY FOLDER "lua")


# 创建子项目——luac
# 生成lua编译器——luac.exe
add_executable(luac ${LUALIB_SRC} src/luac.c)
set_property(TARGET luac PROPERTY FOLDER "lua")
