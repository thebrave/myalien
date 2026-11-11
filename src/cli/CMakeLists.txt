add_executable(myalien main.cpp common.cpp status.cpp profile.cpp gmode.cpp battery.cpp fan.cpp games.cpp powermode.cpp
    ${PROJECT_SOURCE_DIR}/res/cli.rc)
target_link_libraries(myalien PRIVATE myalien-core)
