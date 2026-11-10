add_executable(myalien main.cpp common.cpp status.cpp profile.cpp gmode.cpp)
target_link_libraries(myalien PRIVATE myalien-core)
