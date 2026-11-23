cmake_minimum_required(VERSION 3.18)
project(hsynth VERSION 0.1.0 LANGUAGES CXX)

# Standalone, platform-independent DSP engine. Builds on Android (NDK),
# Linux / Raspberry Pi, Windows (MinGW/MSVC) and macOS.

option(HSYNTH_BUILD_TOOLS "Build offline render / asset tools" ON)
option(HSYNTH_BUILD_TESTS "Build unit tests" ON)
# Build the tests / bench as command-line programs for an Android device
# (adb push + run under /data/local/tmp/hsynth). Not used by the app build.
option(HSYNTH_DEVICE_TESTS "Build hsynth_tests / hsynth_bench for an Android device" OFF)
set(HSYNTH_DEVICE_ROOT "/data/local/tmp/hsynth" CACHE STRING "Device folder that holds tests/data and tests/golden")
option(HSYNTH_ENABLE_SIMD "Allow SIMD-optimised code paths (NEON/SSE). Generic code is always available." ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT DEFINED HSYNTH_PERFORMANCE_SOURCES)
    set(HSYNTH_PERFORMANCE_SOURCES
        src/audio_tap.cpp
        src/metronome.cpp
        src/physical.cpp
        src/piano.cpp
        src/backing_player.cpp
        src/pad_sampler.cpp
        src/live_sampler.cpp
        src/midi_zone.cpp
        src/pcm_file.cpp
        src/performance_sequence.cpp
        src/morph.cpp
        src/npu_morph.cpp
        src/stft.cpp
        src/performance.cpp
        src/performance_input.cpp
        src/tuner.cpp
    )
endif()

if(NOT DEFINED HSYNTH_FORMAT_SOURCES)
    set(HSYNTH_FORMAT_SOURCES
        src/formats/sf2.cpp
        src/formats/dls_gig.cpp
        src/formats/dspreset.cpp
        src/formats/dx7_sysex.cpp
        src/formats/wavetable_ext.cpp
    )
endif()

add_library(hsynth STATIC
    src/engine.cpp
    ${HSYNTH_PERFORMANCE_SOURCES}
    ${HSYNTH_FORMAT_SOURCES}
    src/live_transition.cpp
    src/master_stage.cpp
    src/voice.cpp
    src/oscillators.cpp
    src/fm.cpp
    src/params.cpp
    src/preset.cpp
    src/json.cpp
    src/wav.cpp
    src/flac.cpp
    src/tuning.cpp
    src/resample.cpp
    src/sample_set.cpp
    src/sfz.cpp
    src/midi.cpp
    src/host_utils.cpp
    src/fileio.cpp
    src/worker_pool.cpp
    src/streaming.cpp
    src/preset_index.cpp
    src/midi_learn.cpp
    src/remote.cpp
    src/setup.cpp
    src/ble_midi.cpp
    src/smf.cpp
    src/gm_bank.cpp
    src/wavetable.cpp
    src/fx/convolver.cpp
    src/fx/ir_load.cpp
    src/fx/rack.cpp
    src/fx/insert.cpp
    src/fx/fx_dynamics.cpp
    src/fx/fx_drive.cpp
    src/fx/fx_filter.cpp
    src/fx/fx_mod.cpp
    src/fx/fx_pitch.cpp
    src/fx/fx_time.cpp
    src/fx/fx_vocoder.cpp
    src/fx/fx_extra.cpp
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

find_package(Threads REQUIRED)
target_link_libraries(hsynth PUBLIC Threads::Threads)
if(ANDROID)
    target_link_libraries(hsynth PRIVATE ${CMAKE_DL_LIBS})
endif()

# MinGW: link the C++ runtime statically so the tools do not pick up an
# incompatible libstdc++ DLL from another toolchain on PATH.
if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

if(HSYNTH_BUILD_TOOLS AND (NOT ANDROID OR HSYNTH_DEVICE_TESTS))
    add_executable(hsynth_render tools/hsynth_render.cpp)
    target_link_libraries(hsynth_render PRIVATE hsynth)
    add_executable(hsynth_assets tools/hsynth_assets.cpp)
    target_link_libraries(hsynth_assets PRIVATE hsynth)
    add_executable(hsynth_bench tools/hsynth_bench.cpp)
    target_link_libraries(hsynth_bench PRIVATE hsynth)
    add_executable(hsynth_polybench tools/hsynth_polybench.cpp)
    target_link_libraries(hsynth_polybench PRIVATE hsynth)
    add_executable(hsynth_fuzz tools/hsynth_fuzz.cpp)
    target_link_libraries(hsynth_fuzz PRIVATE hsynth)
    add_executable(hsynth_switch tools/hsynth_switch.cpp)
    target_link_libraries(hsynth_switch PRIVATE hsynth)
    add_executable(hsynth_remote_sim tools/hsynth_remote_sim.cpp)
    target_link_libraries(hsynth_remote_sim PRIVATE hsynth)
endif()

if(HSYNTH_BUILD_TESTS AND (NOT ANDROID OR HSYNTH_DEVICE_TESTS))
    enable_testing()
    add_executable(hsynth_tests tests/test_main.cpp tests/test_dsp.cpp tests/test_engine.cpp tests/test_io.cpp tests/test_fx.cpp tests/test_multi.cpp tests/test_voice.cpp tests/test_sfz.cpp tests/test_flac.cpp tests/test_quality.cpp tests/test_realtime.cpp tests/test_parallel.cpp tests/test_streaming.cpp tests/test_browser.cpp tests/test_fm8.cpp tests/test_fx_extra.cpp tests/test_v07.cpp tests/test_v08_p1.cpp tests/test_v08_p2.cpp tests/test_v08_p3.cpp tests/test_v08_p4.cpp tests/test_v08_p5.cpp tests/test_song.cpp tests/test_remote.cpp tests/test_ble.cpp tests/test_controllers.cpp)
    target_link_libraries(hsynth_tests PRIVATE hsynth Threads::Threads)
    if(ANDROID)
        set(HSYNTH_TESTS_ROOT "${HSYNTH_DEVICE_ROOT}/tests")
    else()
        set(HSYNTH_TESTS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    endif()
    target_compile_definitions(hsynth_tests PRIVATE HSYNTH_TEST_DATA="${HSYNTH_TESTS_ROOT}/data"
                                                    HSYNTH_GOLDEN_DIR="${HSYNTH_TESTS_ROOT}/golden")
    add_test(NAME hsynth_tests COMMAND hsynth_tests)
endif()
