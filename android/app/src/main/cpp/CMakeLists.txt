cmake_minimum_required(VERSION 3.22)
project(hsynth_android LANGUAGES CXX)

# The DSP engine is a separate, platform-independent CMake project.
set(HSYNTH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(HSYNTH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HSYNTH_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine)
set(HSYNTH_PERFORMANCE_SOURCES
    ${HSYNTH_ENGINE_DIR}/src/audio_tap.cpp
    ${HSYNTH_ENGINE_DIR}/src/metronome.cpp
    ${HSYNTH_ENGINE_DIR}/src/physical.cpp
    ${HSYNTH_ENGINE_DIR}/src/piano.cpp
    ${HSYNTH_ENGINE_DIR}/src/backing_player.cpp
    ${HSYNTH_ENGINE_DIR}/src/pad_sampler.cpp
    ${HSYNTH_ENGINE_DIR}/src/live_sampler.cpp
    ${HSYNTH_ENGINE_DIR}/src/midi_zone.cpp
    ${HSYNTH_ENGINE_DIR}/src/pcm_file.cpp
    ${HSYNTH_ENGINE_DIR}/src/performance_sequence.cpp
    ${HSYNTH_ENGINE_DIR}/src/morph.cpp
    ${HSYNTH_ENGINE_DIR}/src/npu_morph.cpp
    ${HSYNTH_ENGINE_DIR}/src/stft.cpp
    ${HSYNTH_ENGINE_DIR}/src/performance.cpp
    ${HSYNTH_ENGINE_DIR}/src/performance_input.cpp
    ${HSYNTH_ENGINE_DIR}/src/tuner.cpp
)
set(HSYNTH_FORMAT_SOURCES
    ${HSYNTH_ENGINE_DIR}/src/formats/sf2.cpp
    ${HSYNTH_ENGINE_DIR}/src/formats/dls_gig.cpp
    ${HSYNTH_ENGINE_DIR}/src/formats/dspreset.cpp
    ${HSYNTH_ENGINE_DIR}/src/formats/dx7_sysex.cpp
    ${HSYNTH_ENGINE_DIR}/src/formats/wavetable_ext.cpp
)
add_subdirectory(${HSYNTH_ENGINE_DIR} ${CMAKE_BINARY_DIR}/hsynth_engine)

# Always optimise the DSP code, also in debug APKs (unoptimised DSP would not
# keep up with real time).
target_compile_options(hsynth PRIVATE -O2)

find_package(oboe REQUIRED CONFIG)

add_library(hsynth_jni SHARED
    alsa_out.cpp
    audio_engine.cpp
    jni_bridge.cpp
)
target_compile_features(hsynth_jni PRIVATE cxx_std_17)
target_compile_options(hsynth_jni PRIVATE -O2 -Wall)
target_link_libraries(hsynth_jni PRIVATE hsynth oboe::oboe log android)
# 16 KB page size compatibility (Android 15+)
target_link_options(hsynth_jni PRIVATE "-Wl,-z,max-page-size=16384")
