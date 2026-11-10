add_executable(myalien-svc main.cpp service.cpp pipe_server.cpp charge_cache.cpp log.cpp)
target_link_libraries(myalien-svc PRIVATE myalien-core shell32)
