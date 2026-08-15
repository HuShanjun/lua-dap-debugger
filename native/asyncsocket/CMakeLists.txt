cmake_minimum_required(VERSION 3.16)

add_library(asyncsocket SHARED asyncsocket.c)
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
endif()
