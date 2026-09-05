# Vendored third-party builds. Currently: Lua 5.1 / 5.2 / 5.3 / 5.4.
#
# Inputs (from parent / cache):
#   LUA_VERSION  — "5.1" | "5.2" | "5.3" | "5.4" (required)
#
# Outputs (PARENT_SCOPE + CACHE):
#   LUA_INC_DIR  — headers for the selected Lua
#   LUA_LIB_DIR  — bin/lua5.x (liblua + lua + luac)

cmake_minimum_required(VERSION 3.16)

if(NOT LUA_VERSION)
    message(FATAL_ERROR "LUA_VERSION is required (5.1, 5.2, 5.3, or 5.4)")
endif()

if(LUA_VERSION STREQUAL "5.1")
    set(_LUA_SUBDIR lua-5.1.5)
    set(_LUA_INC "${CMAKE_CURRENT_SOURCE_DIR}/lua-5.1.5/src")
elseif(LUA_VERSION STREQUAL "5.2")
    set(_LUA_SUBDIR lua-5.2.4)
    set(_LUA_INC "${CMAKE_CURRENT_SOURCE_DIR}/lua-5.2.4/src")
elseif(LUA_VERSION STREQUAL "5.3")
    set(_LUA_SUBDIR lua-5.3.6)
    set(_LUA_INC "${CMAKE_CURRENT_SOURCE_DIR}/lua-5.3.6/src")
elseif(LUA_VERSION STREQUAL "5.4")
    set(_LUA_SUBDIR lua-5.4.8)
    set(_LUA_INC "${CMAKE_CURRENT_SOURCE_DIR}/lua-5.4.8/inc")
else()
    message(FATAL_ERROR "Unsupported LUA_VERSION: ${LUA_VERSION}")
endif()

set(_LUA_OUT "${CMAKE_SOURCE_DIR}/bin/lua${LUA_VERSION}")
file(MAKE_DIRECTORY "${_LUA_OUT}")

# Route this version's artifacts into bin/lua5.x (override root bin/).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${_LUA_OUT})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${_LUA_OUT})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${_LUA_OUT})
foreach(_cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_cfg} _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} ${_LUA_OUT})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CFG} ${_LUA_OUT})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CFG} ${_LUA_OUT})
endforeach()

add_subdirectory(${_LUA_SUBDIR})

# Belt-and-suspenders: pin targets even if a subdir resets globals.
foreach(_tgt IN ITEMS liblua lua luac)
    if(TARGET ${_tgt})
        set_target_properties(${_tgt} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${_LUA_OUT}
            LIBRARY_OUTPUT_DIRECTORY ${_LUA_OUT}
            ARCHIVE_OUTPUT_DIRECTORY ${_LUA_OUT}
            RUNTIME_OUTPUT_DIRECTORY_DEBUG ${_LUA_OUT}
            LIBRARY_OUTPUT_DIRECTORY_DEBUG ${_LUA_OUT}
            ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${_LUA_OUT}
            RUNTIME_OUTPUT_DIRECTORY_RELEASE ${_LUA_OUT}
            LIBRARY_OUTPUT_DIRECTORY_RELEASE ${_LUA_OUT}
            ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${_LUA_OUT}
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${_LUA_OUT}
            LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${_LUA_OUT}
            ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO ${_LUA_OUT}
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${_LUA_OUT}
            LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${_LUA_OUT}
            ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL ${_LUA_OUT})
    endif()
endforeach()

set(LUA_INC_DIR "${_LUA_INC}" PARENT_SCOPE)
set(LUA_LIB_DIR "${_LUA_OUT}" PARENT_SCOPE)
# Cache for tooling / reconfigure; allow -D override on next run.
set(LUA_INC_DIR "${_LUA_INC}" CACHE PATH "Lua include directory" FORCE)
set(LUA_LIB_DIR "${_LUA_OUT}" CACHE PATH "Lua library output directory (bin/lua5.x)" FORCE)

message(STATUS "Lua ${LUA_VERSION}: INC=${_LUA_INC}")
message(STATUS "Lua ${LUA_VERSION}: LIB=${_LUA_OUT}")
