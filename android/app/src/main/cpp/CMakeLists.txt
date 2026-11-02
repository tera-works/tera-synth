cmake_minimum_required(VERSION 3.22)
project(hsynth_android LANGUAGES CXX)

# The DSP engine is a separate, platform-independent CMake project.
set(HSYNTH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(HSYNTH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HSYNTH_ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine)
add_subdirectory(${HSYNTH_ENGINE_DIR} ${CMAKE_BINARY_DIR}/hsynth_engine)

# Always optimise the DSP code, also in debug APKs (unoptimised DSP would not
# keep up with real time).
target_compile_options(hsynth PRIVATE -O2)

find_package(oboe REQUIRED CONFIG)

add_library(hsynth_jni SHARED
    audio_engine.cpp
    jni_bridge.cpp
)
target_compile_features(hsynth_jni PRIVATE cxx_std_17)
target_compile_options(hsynth_jni PRIVATE -O2 -Wall)
target_link_libraries(hsynth_jni PRIVATE hsynth oboe::oboe log android)
# 16 KB page size compatibility (Android 15+)
target_link_options(hsynth_jni PRIVATE "-Wl,-z,max-page-size=16384")
