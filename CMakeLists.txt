# duel-engine (AGPL-3.0-or-later). Build:
#   cmake -B build -DOCGCORE_DIR=<ygopro-core checkout with lua/src submodule> -DDEPS_DIR=<dir with json.hpp, miniz/, sqlite/>
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.16)
project(duel_engine C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(OCGCORE_DIR "" CACHE PATH "ygopro-core source (edo9300/ygopro-core)")
set(DEPS_DIR "" CACHE PATH "json.hpp, miniz/, sqlite/")

# Lua 5.4 compiled as C++ with the engine's customization header in front, same as ygopro-core's own build.
set(LUA_NAMES lapi lauxlib lbaselib lcode lctype ldebug ldo ldump lfunc lgc liolib llex lmathlib lmem lobject
    lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lvm lzio)
set(LUA_SRC)
foreach(n ${LUA_NAMES})
  set(w ${CMAKE_BINARY_DIR}/luawrap/${n}.cpp)
  file(WRITE ${w}.tmp "#include \"${OCGCORE_DIR}/lua/luaconf-customize.h\"\n#include \"${OCGCORE_DIR}/lua/src/${n}.c\"\n")
  configure_file(${w}.tmp ${w} COPYONLY)
  list(APPEND LUA_SRC ${w})
endforeach()
add_library(lua STATIC ${LUA_SRC})
target_include_directories(lua PUBLIC ${OCGCORE_DIR}/lua/src)

set(OCG_NAMES card duel effect field interpreter libcard libdebug libduel libeffect libgroup ocgapi operations
    playerop processor processor_visit scriptlib)
set(OCG_SRC)
foreach(n ${OCG_NAMES})
  list(APPEND OCG_SRC ${OCGCORE_DIR}/${n}.cpp)
endforeach()
add_library(ocgcore STATIC ${OCG_SRC})
target_include_directories(ocgcore PUBLIC ${OCGCORE_DIR})
target_link_libraries(ocgcore PUBLIC lua)

add_library(deps STATIC ${DEPS_DIR}/sqlite/sqlite3.c ${DEPS_DIR}/miniz/miniz.c)
target_include_directories(deps PUBLIC ${DEPS_DIR} ${DEPS_DIR}/sqlite ${DEPS_DIR}/miniz)
target_compile_definitions(deps PUBLIC SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)

add_executable(duel-engine src/main.cpp src/messages.cpp)
target_link_libraries(duel-engine PRIVATE ocgcore deps)
if(MSVC)
  set_property(TARGET duel-engine lua ocgcore deps PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(duel-engine PRIVATE /bigobj /utf-8)
elseif(MINGW)
  target_link_options(duel-engine PRIVATE -static)
endif()
