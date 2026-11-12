cmake_minimum_required(VERSION 3.25)

project(MyAlien
    VERSION 0.1.0
    DESCRIPTION "Thermal and battery care for the Dell G15 5520"
    LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "MyAlien targets Windows with MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: single self-contained executables, no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    MYALIEN_VERSION="${PROJECT_VERSION}"
    MYALIEN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MYALIEN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MYALIEN_VERSION_PATCH=${PROJECT_VERSION_PATCH})

add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8;/EHsc;/Zc:__cplusplus;/MP>")

# Imported DLLs are loaded from System32 only (LOAD_LIBRARY_SEARCH_SYSTEM32), never from the
# executable's own folder: the tray and the CLI may be run from a user-writable folder such as
# Downloads, where a planted version.dll or winsqlite3.dll would otherwise be loaded first.
add_link_options(/DEPENDENTLOADFLAG:0x800)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/cli)
add_subdirectory(src/svc)
add_subdirectory(src/tray)
add_subdirectory(src/setup)
add_subdirectory(tests)
