cmake_minimum_required(VERSION 3.18)
project(hsynth VERSION 0.1.0 LANGUAGES CXX)

# Standalone, platform-independent DSP engine. Builds on Android (NDK),
# Linux / Raspberry Pi, Windows (MinGW/MSVC) and macOS.

option(HSYNTH_BUILD_TOOLS "Build offline render / asset tools" ON)
option(HSYNTH_BUILD_TESTS "Build unit tests" ON)
option(HSYNTH_ENABLE_SIMD "Allow SIMD-optimised code paths (NEON/SSE). Generic code is always available." ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(hsynth STATIC
    src/engine.cpp
    src/voice.cpp
    src/oscillators.cpp
    src/params.cpp
    src/preset.cpp
    src/json.cpp
    src/wav.cpp
    src/sample_set.cpp
    src/sfz.cpp
    src/midi.cpp
    src/host_utils.cpp
    src/wavetable.cpp
    src/fx/convolver.cpp
    src/fx/ir_load.cpp
    src/fx/rack.cpp
    src/fx/fx_dynamics.cpp
    src/fx/fx_drive.cpp
    src/fx/fx_filter.cpp
    src/fx/fx_mod.cpp
    src/fx/fx_pitch.cpp
    src/fx/fx_time.cpp
    src/fx/fx_vocoder.cpp
)
target_include_directories(hsynth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hsynth PUBLIC cxx_std_17)
set_target_properties(hsynth PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(HSYNTH_ENABLE_SIMD)
    target_compile_definitions(hsynth PUBLIC HSYNTH_SIMD=1)
endif()
if(MSVC)
    target_compile_options(hsynth PRIVATE /W4)
else()
    target_compile_options(hsynth PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

find_package(Threads)

# MinGW: link the C++ runtime statically so the tools do not pick up an
# incompatible libstdc++ DLL from another toolchain on PATH.
if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

if(HSYNTH_BUILD_TOOLS AND NOT ANDROID)
    add_executable(hsynth_render tools/hsynth_render.cpp)
    target_link_libraries(hsynth_render PRIVATE hsynth)
    add_executable(hsynth_assets tools/hsynth_assets.cpp)
    target_link_libraries(hsynth_assets PRIVATE hsynth)
    add_executable(hsynth_bench tools/hsynth_bench.cpp)
    target_link_libraries(hsynth_bench PRIVATE hsynth)
endif()

if(HSYNTH_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(hsynth_tests tests/test_main.cpp tests/test_dsp.cpp tests/test_engine.cpp tests/test_io.cpp tests/test_fx.cpp)
    target_link_libraries(hsynth_tests PRIVATE hsynth Threads::Threads)
    target_compile_definitions(hsynth_tests PRIVATE HSYNTH_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
                                                    HSYNTH_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
    add_test(NAME hsynth_tests COMMAND hsynth_tests)
endif()
