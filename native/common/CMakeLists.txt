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
