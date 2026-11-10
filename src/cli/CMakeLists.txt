add_executable(myalien main.cpp status.cpp)
target_link_libraries(myalien PRIVATE myalien-core)
