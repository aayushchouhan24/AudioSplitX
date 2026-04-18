cmake_minimum_required(VERSION 3.24)

project(AudioSplitX VERSION 0.1.0 LANGUAGES CXX)

option(AUDIOSPLITX_STATIC_RUNTIME "Link MSVC runtime statically for single EXE distribution" OFF)

if (MSVC AND AUDIOSPLITX_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(AUDIOSPLITX_CORE_SOURCES
    src/AudioEngine.cpp
    src/AudioFormat.cpp
    src/AudioSampleConvert.cpp
    src/Common.cpp
    src/DeviceEnumerator.cpp
    src/Mmcss.cpp
    src/Resampler.cpp
    src/SyncManager.cpp
    src/WasapiCapture.cpp
    src/WasapiRenderer.cpp
)

add_library(audiosplitx_core STATIC ${AUDIOSPLITX_CORE_SOURCES})

target_compile_features(audiosplitx_core PUBLIC cxx_std_20)

target_compile_definitions(audiosplitx_core PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

if (MSVC)
    target_compile_options(audiosplitx_core PRIVATE /W4 /permissive- /EHsc /utf-8 /wd4324)
endif()

target_link_libraries(audiosplitx_core PUBLIC
    avrt
    mmdevapi
    ole32
    propsys
    uuid
    winmm
)

add_executable(audiosplitx src/main.cpp)
target_link_libraries(audiosplitx PRIVATE audiosplitx_core)

if (MSVC)
    target_compile_options(audiosplitx PRIVATE /W4 /permissive- /EHsc /utf-8 /wd4324)
endif()

set(AUDIOSPLITX_UI_SOURCES
    src/GuiApp.cpp
    src/audiosplitx-ui.rc
)

add_executable(audiosplitx-ui WIN32 ${AUDIOSPLITX_UI_SOURCES})

set_target_properties(audiosplitx-ui PROPERTIES
    OUTPUT_NAME "Audio Split-X"
)

target_include_directories(audiosplitx-ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(audiosplitx-ui PRIVATE
    audiosplitx_core
    comctl32
    dwmapi
    gdiplus
    gdi32
    shell32
    user32
)

if (MSVC)
    target_compile_options(audiosplitx-ui PRIVATE /W4 /permissive- /EHsc /utf-8 /wd4324)
endif()
