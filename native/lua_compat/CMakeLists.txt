# Lua 5.1–5.4 compatibility shims (standalone static library).
# Uses LUA_INC_DIR from root / 3rd.

add_library(lua_compat STATIC lua_compat.c)
target_include_directories(lua_compat
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${LUA_INC_DIR})
target_link_directories(lua_compat PRIVATE ${LUA_LIB_DIR})
if(TARGET liblua)
    target_link_libraries(lua_compat PUBLIC liblua)
endif()
set_property(TARGET lua_compat PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET lua_compat PROPERTY FOLDER "native")
