# Select Lua 5.1–5.4 and produce targets liblua + lua.
#
# Resolution:
#   1. LUA_ROOT set  → source tree (build) or install prefix (imported)
#   2. LUA_VERSION=5.4 → vendored 3rd/lua-5.4.8
#   3. else FetchContent official lua.org tarball into the build dir (not git)
#
# Do not treat 3rd/lua-5.1.5 / 5.2.4 / 5.3.6 as vendored sources (those trees
# may contain only a CMakeLists.txt in git).

set(LUA_VERSION "5.4" CACHE STRING "Lua version: 5.1 5.2 5.3 5.4")
set_property(CACHE LUA_VERSION PROPERTY STRINGS 5.1 5.2 5.3 5.4)
set(LUA_ROOT "" CACHE PATH "Optional external Lua prefix or source tree")

if(NOT LUA_VERSION MATCHES "^5\\.[1-4]$")
    message(FATAL_ERROR "LUA_VERSION must be 5.1, 5.2, 5.3, or 5.4 (got '${LUA_VERSION}')")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/BuildLuaFromSource.cmake")

set(_lua_patch "")
if(LUA_VERSION STREQUAL "5.1")
    set(_lua_patch "5.1.5")
elseif(LUA_VERSION STREQUAL "5.2")
    set(_lua_patch "5.2.4")
elseif(LUA_VERSION STREQUAL "5.3")
    set(_lua_patch "5.3.6")
else()
    set(_lua_patch "5.4.8")
endif()

function(_lua_import_from_prefix ROOT)
    set(_inc "")
    foreach(_cand
            "${ROOT}/include"
            "${ROOT}/inc"
            "${ROOT}/src"
            "${ROOT}/include/lua${LUA_VERSION}"
            "${ROOT}/include/lua")
        if(EXISTS "${_cand}/lua.h")
            set(_inc "${_cand}")
            break()
        endif()
    endforeach()
    if(NOT _inc)
        message(FATAL_ERROR "LUA_ROOT='${ROOT}' has no lua.h (tried include/, inc/, src/)")
    endif()

    string(REPLACE "." "" _ver_nodot "${LUA_VERSION}")
    find_library(LUA_LIBRARY
        NAMES lua liblua
              lua${LUA_VERSION} lua${_ver_nodot}
              lua51 lua52 lua53 lua54
        PATHS "${ROOT}/lib" "${ROOT}/lib64" "${ROOT}"
        NO_DEFAULT_PATH)
    if(NOT LUA_LIBRARY)
        message(FATAL_ERROR "LUA_ROOT='${ROOT}' has no Lua library under lib/")
    endif()

    add_library(liblua UNKNOWN IMPORTED GLOBAL)
    set_target_properties(liblua PROPERTIES
        IMPORTED_LOCATION "${LUA_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${_inc}")

    if(WIN32)
        set(_exe "${ROOT}/bin/lua.exe")
    else()
        set(_exe "${ROOT}/bin/lua")
    endif()
    if(EXISTS "${_exe}")
        add_executable(lua IMPORTED GLOBAL)
        set_target_properties(lua PROPERTIES IMPORTED_LOCATION "${_exe}")
    endif()

    set(LUA_INCLUDE_DIR "${_inc}" PARENT_SCOPE)
endfunction()

set(LUA_INCLUDE_DIR "")

if(LUA_ROOT)
    get_filename_component(_lua_root_abs "${LUA_ROOT}" ABSOLUTE)
    if(NOT EXISTS "${_lua_root_abs}")
        message(FATAL_ERROR "LUA_ROOT does not exist: ${LUA_ROOT}")
    endif()
    if(EXISTS "${_lua_root_abs}/src/lapi.c" OR EXISTS "${_lua_root_abs}/lapi.c")
        message(STATUS "Lua ${LUA_VERSION}: building from LUA_ROOT=${_lua_root_abs}")
        lua_build_from_source("${_lua_root_abs}")
    else()
        message(STATUS "Lua ${LUA_VERSION}: importing prefix LUA_ROOT=${_lua_root_abs}")
        _lua_import_from_prefix("${_lua_root_abs}")
    endif()
elseif(LUA_VERSION STREQUAL "5.4")
    set(_vendored "${CMAKE_SOURCE_DIR}/3rd/lua-5.4.8")
    if(NOT EXISTS "${_vendored}/src" OR NOT EXISTS "${_vendored}/CMakeLists.txt")
        message(FATAL_ERROR "Vendored Lua 5.4.8 missing at ${_vendored}")
    endif()
    message(STATUS "Lua 5.4: vendored ${_vendored}")
    add_subdirectory("${_vendored}" "${CMAKE_BINARY_DIR}/lua-5.4.8")
    set(LUA_INCLUDE_DIR "${_vendored}/inc")
    if(TARGET liblua)
        target_include_directories(liblua PUBLIC
            "${_vendored}/inc"
            "${_vendored}/src")
    endif()
else()
    # Directory-scope FetchContent: official tarball has no CMakeLists.
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    set(_lua_url "https://www.lua.org/ftp/lua-${_lua_patch}.tar.gz")
    message(STATUS "Fetching Lua ${_lua_patch} from ${_lua_url} (build dir, not git)")
    FetchContent_Declare(lua_src
        URL "${_lua_url}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    # Official tarballs have no CMakeLists; MakeAvailable only populates.
    FetchContent_MakeAvailable(lua_src)
    lua_build_from_source("${lua_src_SOURCE_DIR}")
endif()

if(NOT LUA_INCLUDE_DIR)
    message(FATAL_ERROR "LuaSelect did not set LUA_INCLUDE_DIR")
endif()
if(NOT TARGET liblua)
    message(FATAL_ERROR "LuaSelect did not create target liblua")
endif()

set(LUA_INCLUDE_DIR "${LUA_INCLUDE_DIR}" CACHE PATH "Include dir of the selected Lua" FORCE)
message(STATUS "Lua ${LUA_VERSION} (${_lua_patch}) include: ${LUA_INCLUDE_DIR}")
