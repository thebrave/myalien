add_library(myalien-core STATIC
    actions.cpp
    error.cpp
    wmi.cpp
    awcc.cpp
    bios_attributes.cpp
    battery.cpp
    charge.cpp
    ipc.cpp
    snapshot.cpp
    system.cpp
    text_formats.cpp
    games.cpp)

target_include_directories(myalien-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(myalien-core PUBLIC
    wbemuuid ole32 oleaut32 setupapi advapi32
    shlwapi xmllite winsqlite3 version)

# The game catalog runs once per scan and is string-heavy: optimize it for size, not speed.
set_source_files_properties(games.cpp text_formats.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Release>:/O1>")
