cmake_minimum_required(VERSION 3.16)

add_library(asyncsocket SHARED asyncsocket.c poll_loop.c)
target_include_directories(asyncsocket PRIVATE
    ${CMAKE_SOURCE_DIR}/3rd/lua-5.4.8/inc)

if(TARGET liblua)
    target_link_libraries(asyncsocket PRIVATE liblua)
endif()

set_target_properties(asyncsocket PROPERTIES
    OUTPUT_NAME "asyncsocket"
    PREFIX "")

if(WIN32)
    target_link_libraries(asyncsocket PRIVATE ws2_32)
    target_compile_definitions(asyncsocket PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0600
        _CRT_SECURE_NO_WARNINGS)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(asyncsocket PRIVATE Threads::Threads)
endif()
