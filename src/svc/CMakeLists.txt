add_executable(myalien-svc main.cpp service.cpp pipe_server.cpp charge_cache.cpp monitor.cpp thermal_state.cpp log.cpp
    ${PROJECT_SOURCE_DIR}/res/svc.rc)
target_link_libraries(myalien-svc PRIVATE myalien-core shell32)
