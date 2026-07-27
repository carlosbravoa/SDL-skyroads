cmake_minimum_required(VERSION 3.16)
project(skyroads_c VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# All headers are addressed as "data/foo.hpp", "core/bar.hpp", etc.
include_directories(${CMAKE_SOURCE_DIR}/src)

# ---- data library: zero-dependency, integer-exact asset loaders ----------
add_library(skyroads_data
  src/data/assets.cpp
  src/data/byteio.cpp
  src/data/compression.cpp
  src/data/cfg.cpp
  src/data/image.cpp
  src/data/sound.cpp
  src/data/dashboard.cpp
  src/data/demo.cpp
  src/data/config.cpp
  src/data/roads.cpp
  src/data/level.cpp
  src/data/trekdat.cpp
  src/data/muzax.cpp
  src/data/exe.cpp
)

# ---- core library: deterministic simulation / app state ------------------
add_library(skyroads_core
  src/core/gameplay.cpp
  src/core/dos_render_tables.cpp
  src/core/planner.cpp
  src/core/app.cpp
)
target_link_libraries(skyroads_core PUBLIC skyroads_data)

# ---- renderer library: 320x200 software reference renderer ------------
add_library(skyroads_renderer
  src/renderer/renderer.cpp
)
target_link_libraries(skyroads_renderer PUBLIC skyroads_core skyroads_data)

# ---- ymfm: emulation of the YM3812 (OPL2) the game drives ------------------
# Vendored subset of https://github.com/aaronsgiles/ymfm (BSD-3-Clause).
add_library(ymfm STATIC
  third_party/ymfm/ymfm_opl.cpp
  third_party/ymfm/ymfm_adpcm.cpp
  third_party/ymfm/ymfm_pcm.cpp
)
target_include_directories(ymfm PUBLIC third_party/ymfm)
# Upstream code, not ours to lint.
target_compile_options(ymfm PRIVATE -w)

# ---- audio library: the game's OPL2 driver + mixer -------------------------
add_library(skyroads_audio
  src/audio/audio.cpp
  src/audio/opl_chip.cpp
)
target_link_libraries(skyroads_audio PUBLIC skyroads_core skyroads_data PRIVATE ymfm)

# ---- tests -----------------------------------------------------------------
enable_testing()

add_executable(test_data tests/test_data.cpp)
target_link_libraries(test_data PRIVATE skyroads_data)
add_test(NAME data COMMAND test_data)

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE skyroads_core)
add_test(NAME core COMMAND test_core)

add_executable(test_renderer tests/test_renderer.cpp)
target_link_libraries(test_renderer PRIVATE skyroads_renderer)
add_test(NAME renderer COMMAND test_renderer)

add_executable(test_audio tests/test_audio.cpp)
target_link_libraries(test_audio PRIVATE skyroads_audio)
add_test(NAME audio COMMAND test_audio)

# The tests read the real game data. Point them at it explicitly so they pass from any
# build directory, not just one nested inside the source tree (check.hpp otherwise
# falls back to ../skyroads-assets). SKYROADS_ASSETS still overrides.
set_tests_properties(data core renderer audio PROPERTIES
  ENVIRONMENT "SKYROADS_ASSETS=${CMAKE_SOURCE_DIR}/skyroads-assets")

# ---- CLI verifier ----------------------------------------------------------
add_executable(skyroads-cli src/cli/main.cpp)
target_link_libraries(skyroads-cli PRIVATE skyroads_core)

# ---- SDL host (optional: only built when SDL2 is available) -----------------
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
  endif()
endif()

if(TARGET SDL2::SDL2 OR TARGET PkgConfig::SDL2)
  add_executable(skyroads-sdl src/platform/main.cpp)
  target_link_libraries(skyroads-sdl PRIVATE skyroads_renderer skyroads_audio)
  if(TARGET SDL2::SDL2)
    target_link_libraries(skyroads-sdl PRIVATE SDL2::SDL2)
  else()
    target_link_libraries(skyroads-sdl PRIVATE PkgConfig::SDL2)
  endif()
  message(STATUS "SDL2 found: building skyroads-sdl host")
else()
  message(STATUS "SDL2 not found: skipping skyroads-sdl host")
endif()
