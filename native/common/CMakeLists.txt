# Compile lua_compat.c once. asyncsocket (static + shared) and luadap link it.
# Compiling the .c into both asyncsocket_static and luadap duplicates 5.1 symbols.

add_library(lua_compat STATIC lua_compat.c)
target_include_directories(lua_compat
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${LUA_INCLUDE_DIR})
if(TARGET liblua)
    target_link_libraries(lua_compat PUBLIC liblua)
endif()
set_property(TARGET lua_compat PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET lua_compat PROPERTY FOLDER "native")

add_library(circle_buffer STATIC circle_buffer.c)
target_include_directories(circle_buffer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(circle_buffer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    target_compile_options(circle_buffer PRIVATE /std:c11 /experimental:c11atomics)
endif()
set_property(TARGET circle_buffer PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET circle_buffer PROPERTY FOLDER "native")

add_executable(circle_buffer_test circle_buffer_test.c)
target_link_libraries(circle_buffer_test PRIVATE circle_buffer)
set_property(TARGET circle_buffer_test PROPERTY FOLDER "native")
