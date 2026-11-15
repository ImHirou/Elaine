cmake_minimum_required(VERSION 3.25)
project(craftc VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------- options
# The code name appears here (and in docs) only; sources use layer namespaces, so renaming the
# project is a one-line change.
set(MC_EXE "craftc" CACHE STRING "Name of the server binary")
set(MC_ARCH "x86-64-v3" CACHE STRING
    "-march baseline. x86-64-v3 = AVX2+BMI2+FMA (Haswell/Zen1 and newer). 'native' = this machine only")
option(MC_LTO "Link-time optimization in optimized builds" ON)
option(MC_TESTS "Build unit tests" ON)
option(MC_BENCH "Build micro-benchmarks" ON)
option(MC_FRAME_POINTERS "Keep frame pointers (cheap, makes perf call graphs reliable)" OFF)
set(MC_SANITIZE "" CACHE STRING "Sanitizers for dev builds, e.g. address,undefined")

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
  target_compile_options(mc_flags INTERFACE -fsanitize=${MC_SANITIZE} -fno-sanitize=vptr -fno-omit-frame-pointer -g)
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

# ---------------------------------------------------------------------------- dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing>=2.4)
pkg_check_modules(DEFLATE REQUIRED IMPORTED_TARGET libdeflate)
pkg_check_modules(YYJSON REQUIRED IMPORTED_TARGET yyjson)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------- targets
add_library(core STATIC
  src/base/log.cpp
  src/proto/wire.cpp
  src/proto/frame.cpp
  src/proto/static_data.cpp
  src/nbt/nbt.cpp
  src/text/component.cpp
  src/text/text.cpp
  src/crypto/cfb8.cpp
  src/crypto/auth.cpp
  src/net/reactor.cpp
  src/server/settings.cpp
  src/server/session_auth.cpp
  src/world/block.cpp
  src/world/section.cpp
  src/world/light.cpp
  src/world/chunk_codec.cpp
  src/world/generator.cpp
  src/world/dimension.cpp
  src/world/collide.cpp
  src/item/components.cpp
  src/item/stack.cpp
  src/item/recipes.cpp
  src/entity/store.cpp
  src/entity/tracker.cpp
  src/server/prelogin.cpp
  src/server/block_entity.cpp
  src/server/inventory.cpp
  src/server/inventory_actions.cpp
  src/server/item_entities.cpp
  src/server/block_actions.cpp
  src/server/block_entities.cpp
  src/server/explosion.cpp
  src/server/players.cpp
  src/server/command.cpp
  src/server/command_args.cpp
  src/server/commands.cpp
  src/server/console.cpp
  src/server/game.cpp
)
target_link_libraries(core
  PUBLIC mc_flags PkgConfig::URING Threads::Threads
  PRIVATE mc_core_flags PkgConfig::DEFLATE PkgConfig::YYJSON OpenSSL::Crypto CURL::libcurl)

add_executable(server_exe src/main.cpp)
set_target_properties(server_exe PROPERTIES OUTPUT_NAME ${MC_EXE})
target_link_libraries(server_exe PRIVATE core mc_core_flags)

if(MC_TESTS)
  find_package(GTest)
  if(GTest_FOUND)
    enable_testing()
    add_executable(unit_tests
      tests/unit/varint_test.cpp
      tests/unit/wire_test.cpp
      tests/unit/nbt_test.cpp
      tests/unit/text_test.cpp
      tests/unit/frame_test.cpp
      tests/unit/crypto_test.cpp
      tests/unit/spsc_test.cpp
      tests/unit/world_test.cpp
      tests/unit/light_test.cpp
      tests/unit/entity_test.cpp
      tests/unit/item_test.cpp
      tests/unit/inventory_test.cpp
      tests/unit/container_test.cpp
      tests/unit/command_test.cpp
    )
    target_link_libraries(unit_tests PRIVATE core GTest::gtest_main OpenSSL::Crypto)
    add_test(NAME unit COMMAND unit_tests)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
      add_test(NAME integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/run.py $<TARGET_FILE:server_exe>)
      set_tests_properties(integration PROPERTIES TIMEOUT 240)
    endif()
  else()
    message(WARNING "GoogleTest not found: unit tests are not built")
  endif()
endif()

if(MC_BENCH)
  add_executable(bench bench/bench.cpp bench/world_bench.cpp bench/entity_bench.cpp bench/item_bench.cpp
    bench/nbt_bench.cpp bench/game_bench.cpp bench/command_bench.cpp)
  target_link_libraries(bench PRIVATE core mc_core_flags OpenSSL::Crypto)
endif()
