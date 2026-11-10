add_library(myalien-core STATIC
    error.cpp
    wmi.cpp
    awcc.cpp
    bios_attributes.cpp
    battery.cpp
    charge.cpp
    system.cpp)

target_include_directories(myalien-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(myalien-core PUBLIC
    wbemuuid ole32 oleaut32 setupapi advapi32)
