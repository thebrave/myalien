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
    system.cpp)

target_include_directories(myalien-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(myalien-core PUBLIC
    wbemuuid ole32 oleaut32 setupapi advapi32)
