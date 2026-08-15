cmake_minimum_required(VERSION 3.16)

project(lua-dap-debugger LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 全局设置输出路径

# 所有生成的库和可执行文件输出到 bin 目录下
set(OUTPUT_BIN_DIR ${CMAKE_SOURCE_DIR}/bin)
# 单配置生成器
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_BIN_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_BIN_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${OUTPUT_BIN_DIR})

# 多配置生成器（如 Visual Studio）
foreach(CONFIG IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${CONFIG} _CONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CONFIG} ${OUTPUT_BIN_DIR})
endforeach()


if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# set(PROJECT_SOURCES
#     src/main.cpp
# )

# add_executable(lua-dap-debugger ${PROJECT_SOURCES})

# target_include_directories(lua-dap-debugger
#     PRIVATE
#         ${CMAKE_CURRENT_SOURCE_DIR}/include
# )

include_directories(3rd/lua-5.4.8/inc)
include_directories(3rd/sol2/include)
include_directories(3rd/nlohmann/include)

link_directories(bin)

add_subdirectory(3rd/lua-5.4.8)
add_subdirectory(3rd/luasocket)
add_subdirectory(main)

# 后续可以在这里添加库、子目录和测试
