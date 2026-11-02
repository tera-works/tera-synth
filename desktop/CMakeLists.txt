cmake_minimum_required(VERSION 3.18)
project(hsynth_desktop LANGUAGES C CXX)

# Native desktop host for developing and testing HSynth without an Android
# device. Windows today (WASAPI + winmm + Win32/OpenGL); the audio layer
# (miniaudio), UI (Dear ImGui) and library code are portable, so a Linux /
# Raspberry Pi variant only needs a GLFW/SDL main and an ALSA MIDI input.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(HSYNTH_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(HSYNTH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HSYNTH_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(${HSYNTH_ROOT}/engine ${CMAKE_BINARY_DIR}/engine)

set(IMGUI_DIR ${HSYNTH_ROOT}/third_party/imgui-1.91.8)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

if(WIN32)
    add_executable(hsynth_desktop WIN32
        src/main_win32.cpp
        src/app.cpp
        src/audio_out.cpp
        src/miniaudio_impl.cpp
        src/midi_in_win32.cpp
        src/host_library.cpp
        src/platform_win32.cpp
    )
    target_include_directories(hsynth_desktop PRIVATE src ${HSYNTH_ROOT}/third_party/miniaudio)
    target_compile_definitions(hsynth_desktop PRIVATE
        UNICODE _UNICODE NOMINMAX
        HSYNTH_DEFAULT_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
        HSYNTH_DEFAULT_FACTORY_DIR="${HSYNTH_ROOT}/android/app/src/main/assets")
    target_link_libraries(hsynth_desktop PRIVATE hsynth imgui opengl32 gdi32 dwmapi winmm ole32 comdlg32 shell32 uuid imm32)
    if(MINGW)
        # static runtime: no dependency on whichever libstdc++ DLL is on PATH
        # -municode: wWinMain entry point
        target_link_options(hsynth_desktop PRIVATE -static -municode)
    endif()

    # Automatic multisampler (SoundFont rendering or live MIDI + audio capture).
    # Produces test content only; see docs/SAMPLER.md.
    add_executable(hsynth_sampler
        src/sampler/hsynth_sampler.cpp
        src/sampler/sampler_core.cpp
        src/sampler/ins_file.cpp
        src/sampler/source_sf2.cpp
        src/sampler/source_live_win32.cpp
        src/miniaudio_impl.cpp
    )
    target_include_directories(hsynth_sampler PRIVATE src ${HSYNTH_ROOT}/third_party/miniaudio ${HSYNTH_ROOT}/third_party/tinysoundfont)
    target_compile_definitions(hsynth_sampler PRIVATE NOMINMAX HSYNTH_DESKTOP_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(hsynth_sampler PRIVATE hsynth winmm ole32)
    if(MINGW)
        target_link_options(hsynth_sampler PRIVATE -static)
    endif()
else()
    message(WARNING "hsynth_desktop: only the Windows host is implemented so far")
endif()
