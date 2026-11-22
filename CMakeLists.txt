# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(elaine VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------- options
# The project's name appears here (and in docs) only; sources use layer namespaces, so renaming the
# project is a one-line change.
set(MC_EXE "elaine" CACHE STRING "Name of the server binary")
set(MC_ARCH "x86-64-v3" CACHE STRING
    "-march baseline. x86-64-v3 = AVX2+BMI2+FMA (Haswell/Zen1 and newer). 'native' = this machine only")
option(MC_LTO "Link-time optimization in optimized builds" ON)
option(MC_LTO_CACHE "Keep what LTO compiled between links (GCC 15+): a link redoes only what an edit reached" ON)
option(MC_PCH "Precompiled headers: server/game.hpp for the files of the game, gtest for the unit tests" ON)
option(MC_TESTS "Build unit tests" ON)
option(MC_BENCH "Build micro-benchmarks" ON)
option(MC_FRAME_POINTERS "Keep frame pointers (cheap, makes perf call graphs reliable)" OFF)
set(MC_SANITIZE "" CACHE STRING "Sanitizers for dev builds, e.g. address,undefined")
set(MC_DEBUG_LEVEL "1" CACHE STRING
    "-g level of sanitizer builds: 1 = functions and lines, all a sanitizer report reads; 2 = locals and types too, for gdb")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)          # gnu++23: #embed, __attribute__((assume)), statement attributes
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  message(FATAL_ERROR "Linux x86-64 only for now: the reactor is io_uring and the hot paths use AVX2/BMI2/AES-NI")
endif()

# ---------------------------------------------------------------------------- flags
add_library(mc_flags INTERFACE)
target_compile_options(mc_flags INTERFACE
  -march=${MC_ARCH}
  -maes -mpclmul                      # not implied by x86-64-v3; AES-CFB8 needs AES-NI
  -Wall -Wextra -Wshadow -Wno-unused-parameter
  -fno-plt -fno-semantic-interposition -fvisibility=hidden
  -fno-math-errno -fno-trapping-math  # NOT -ffast-math: game physics must match vanilla's IEEE results
  -ffp-contract=off                   # ...which also rules out fusing a*b+c into an FMA: Java never does
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(mc_flags INTERFACE -Wno-c23-extensions)  # #embed in C++23 mode
endif()
if(MC_FRAME_POINTERS)
  target_compile_options(mc_flags INTERFACE -fno-omit-frame-pointer -g)
endif()
if(MC_SANITIZE)
  # No vptr check: it needs RTTI, which the server is built without; the tests (built with RTTI)
  # would then ask for typeinfo of server classes that has never been emitted.
  # -g1 unless asked otherwise: a report's frames need functions and line tables, and the rest of -g is
  # 7-20% of a compile (game.cpp 6.8 -> 6.3 s, entity_test.cpp 10.0 -> 8.0 s). It stands after the -g of
  # CMAKE_CXX_FLAGS_DEBUG on the command line, and of several -g options the last is the one that counts.
  target_compile_options(mc_flags INTERFACE
    -fsanitize=${MC_SANITIZE} -fno-sanitize=vptr -fno-omit-frame-pointer -g${MC_DEBUG_LEVEL})
  target_link_options(mc_flags INTERFACE -fsanitize=${MC_SANITIZE})
endif()
target_include_directories(mc_flags INTERFACE ${CMAKE_SOURCE_DIR}/src)

# Server code is built without exceptions and RTTI (smaller, faster, and LuaJIT unwinding through
# C++ frames is off the table anyway). Tests keep the defaults because GoogleTest wants them.
add_library(mc_core_flags INTERFACE)
target_compile_options(mc_core_flags INTERFACE -fno-exceptions -fno-rtti)

if(MC_LTO AND NOT MC_SANITIZE AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT mc_ipo OUTPUT mc_ipo_msg)
  if(mc_ipo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(WARNING "LTO not available: ${mc_ipo_msg}")
  endif()
endif()

# An LTO link compiles the whole program again. With a cache GCC keeps the code it made of each partition
# and compiles only the partitions an edit reached - the server, linked alone on four cores, 10.3 s without:
# 0.6 s when nothing changed, 5.6 s after an edit in item_entities.cpp, 9.6 s after one in game.cpp. The
# binary is the one a link without the cache makes, byte for byte. One directory per binary (their
# partitions have nothing in common), made before every link: lto-wrapper stops at a missing directory
# instead of making it, and a cache is a thing people delete.
function(mc_lto_cache target)
  if(MC_LTO_CACHE AND CMAKE_INTERPROCEDURAL_OPTIMIZATION AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
     AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    set(dir ${CMAKE_BINARY_DIR}/lto-cache/${target})
    add_custom_command(TARGET ${target} PRE_LINK COMMAND ${CMAKE_COMMAND} -E make_directory ${dir} VERBATIM)
    target_link_options(${target} PRIVATE -flto-incremental=${dir})
  endif()
endfunction()

# ---------------------------------------------------------------------------- dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing>=2.4)
pkg_check_modules(DEFLATE REQUIRED IMPORTED_TARGET libdeflate)
pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------- generated tables
# src/proto/gen/v<protocol>/ is Mojang's data: tools/mcgen makes it from the jar on every machine, and
# git keeps only its lock (v<protocol>.sha256). The directory current.hpp names must be there, and -
# checked before anything is compiled, on every build - be what the lock says: after a pull that moved
# the lock, a build stops and says so instead of compiling against the tables of another commit.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(STRINGS ${CMAKE_SOURCE_DIR}/src/proto/gen/current.hpp mc_gen_line REGEX "#define PROTO_GEN_DIR ")
string(REGEX REPLACE ".*\"(.+)\".*" "\\1" MC_GEN_DIR "${mc_gen_line}")
if(NOT IS_DIRECTORY ${CMAKE_SOURCE_DIR}/src/${MC_GEN_DIR})
  message(FATAL_ERROR "src/${MC_GEN_DIR}/ is not there: it is generated from Mojang's jar, not kept in git.\n"
    "Run ./install.sh (everything, once) or: python3 tools/mcgen/mcgen.py setup")
endif()
option(MC_GENERATED_CHECK "Stop a build whose generated tables are not what their lock says" ON)
if(MC_GENERATED_CHECK)
  add_custom_target(generated_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mcgen/mcgen.py check --quiet
    COMMENT "Generated tables against their lock" VERBATIM)
endif()
function(mc_needs_generated target)
  if(MC_GENERATED_CHECK)
    add_dependencies(${target} generated_check)
  endif()
endfunction()

# ---------------------------------------------------------------------------- targets
# The files of the game itself, each of which includes server/game.hpp: two seconds of parsing (the
# standard library, the generated tables, every part of the game's state) that MC_PCH makes once, as a
# precompiled header - 2.7 s -> 1.1 s for chat.cpp, 2.1 s -> 0.5 s for time.cpp (gcc 16.2, 2026-11-22).
# The other files of the core compile without it, as they would without the option.
set(MC_GAME_SOURCES
  src/server/block_actions.cpp
  src/server/block_entities.cpp
  src/server/block_shapes.cpp
  src/server/block_updates.cpp
  src/server/chat.cpp
  src/server/chunks.cpp
  src/server/commands.cpp
  src/server/entity_tags.cpp
  src/server/explosion.cpp
  src/server/falling.cpp
  src/server/game.cpp
  src/server/inventory_actions.cpp
  src/server/item_entities.cpp
  src/server/persist.cpp
  src/server/physics.cpp
  src/server/players.cpp
  src/server/saved_tags.cpp
  src/server/signs.cpp
  src/server/time.cpp
)
add_library(core STATIC
  ${MC_GAME_SOURCES}
  src/base/log.cpp
  src/base/java_number.cpp
  src/base/workers.cpp
  src/proto/wire.cpp
  src/proto/frame.cpp
  src/proto/static_data.cpp
  src/nbt/nbt.cpp
  src/nbt/snbt.cpp
  src/nbt/ops.cpp
  src/text/component.cpp
  src/text/text.cpp
  src/text/nbt_text.cpp
  src/crypto/cfb8.cpp
  src/crypto/auth.cpp
  src/net/reactor.cpp
  src/server/settings.cpp
  src/server/session_auth.cpp
  src/world/block.cpp
  src/world/section.cpp
  src/world/light.cpp
  src/world/chunk.cpp
  src/world/chunk_codec.cpp
  src/world/chunk_io.cpp
  src/world/region.cpp
  src/world/store.cpp
  src/world/ticks.cpp
  src/world/generator.cpp
  src/world/dimension.cpp
  src/world/collide.cpp
  src/phys/physics.cpp
  src/item/components.cpp
  src/item/codec.cpp
  src/item/stack.cpp
  src/item/recipes.cpp
  src/entity/store.cpp
  src/entity/tracker.cpp
  src/server/prelogin.cpp
  src/server/block_entity.cpp
  src/server/import.cpp
  src/server/files.cpp
  src/server/player_data.cpp
  src/server/world_setup.cpp
  src/server/inventory.cpp
  src/server/clock.cpp
  src/server/command.cpp
  src/server/command_args.cpp
  src/server/console.cpp
)
target_link_libraries(core
  PUBLIC mc_flags PkgConfig::URING Threads::Threads
  PRIVATE mc_core_flags PkgConfig::DEFLATE PkgConfig::YYJSON OpenSSL::Crypto CURL::libcurl)
mc_needs_generated(core)
if(MC_PCH)
  target_precompile_headers(core PRIVATE ${CMAKE_SOURCE_DIR}/src/server/game.hpp)
  get_target_property(mc_plain_sources core SOURCES)
  list(REMOVE_ITEM mc_plain_sources ${MC_GAME_SOURCES})
  set_source_files_properties(${mc_plain_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

add_executable(server_exe src/main.cpp)
set_target_properties(server_exe PROPERTIES OUTPUT_NAME ${MC_EXE})
target_link_libraries(server_exe PRIVATE core mc_core_flags)
mc_needs_generated(server_exe)
mc_lto_cache(server_exe)

if(MC_TESTS)
  find_package(GTest)
  if(GTest_FOUND)
    enable_testing()
    add_executable(unit_tests
      tests/unit/varint_test.cpp
      tests/unit/wire_test.cpp
      tests/unit/nbt_test.cpp
      tests/unit/snbt_test.cpp
      tests/unit/text_test.cpp
      tests/unit/frame_test.cpp
      tests/unit/crypto_test.cpp
      tests/unit/spsc_test.cpp
      tests/unit/world_test.cpp
      tests/unit/store_test.cpp
      tests/unit/player_data_test.cpp
      tests/unit/ticks_test.cpp
      tests/unit/physics_test.cpp
      tests/unit/light_test.cpp
      tests/unit/entity_test.cpp
      tests/unit/item_test.cpp
      tests/unit/inventory_test.cpp
      tests/unit/container_test.cpp
      tests/unit/command_test.cpp
      tests/unit/chat_test.cpp
      tests/unit/time_test.cpp
    )
    target_link_libraries(unit_tests PRIVATE core GTest::gtest_main OpenSSL::Crypto)
    if(MC_PCH)
      target_precompile_headers(unit_tests PRIVATE <gtest/gtest.h>)  # a second of every test file
    endif()
    mc_needs_generated(unit_tests)
    mc_lto_cache(unit_tests)
    add_test(NAME unit COMMAND unit_tests)
    add_test(NAME integration
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/run.py $<TARGET_FILE:server_exe> -j 0)
    set_tests_properties(integration PROPERTIES TIMEOUT 480)
  else()
    message(WARNING "GoogleTest not found: unit tests are not built")
  endif()
endif()

if(MC_BENCH)
  add_executable(bench bench/bench.cpp bench/world_bench.cpp bench/entity_bench.cpp bench/item_bench.cpp
    bench/nbt_bench.cpp bench/game_bench.cpp bench/command_bench.cpp bench/physics_bench.cpp)
  target_link_libraries(bench PRIVATE core mc_core_flags OpenSSL::Crypto)
  mc_needs_generated(bench)
  mc_lto_cache(bench)
endif()
