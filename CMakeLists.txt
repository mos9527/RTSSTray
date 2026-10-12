cmake_minimum_required(VERSION 3.31)
project(RTSSTray VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "RTSSTray requires Windows 10 or newer.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RTSSTRAY_STATIC_CRT "Statically link the MSVC runtime" ON)
if(MSVC)
    if(RTSSTRAY_STATIC_CRT)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    add_compile_options(/utf-8 /Zc:preprocessor)
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

add_subdirectory(contrib)

add_library(rtss_core STATIC
    src/msiab.cpp
    src/config.cpp
)
target_include_directories(rtss_core PUBLIC src)
target_link_libraries(rtss_core PUBLIC nlohmann_json::nlohmann_json PRIVATE shell32 ole32)

add_executable(RTSSTray WIN32
    src/main.cpp
    src/tray.cpp
    src/frontend.cpp
    resources/app.manifest
)
target_link_libraries(RTSSTray PRIVATE rtss_core ImGuiWin32DX11 user32 gdi32 shell32 ole32)
if(MSVC)
    target_compile_options(rtss_core PRIVATE /W4 /permissive-)
    target_compile_options(RTSSTray PRIVATE /W4 /permissive-)
endif()

install(TARGETS RTSSTray RUNTIME DESTINATION .)
