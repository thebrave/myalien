add_executable(charge_plan_test charge_plan_test.cpp)
target_link_libraries(charge_plan_test PRIVATE myalien-core)
add_test(NAME charge_plan COMMAND charge_plan_test)

add_executable(actions_test actions_test.cpp)
target_link_libraries(actions_test PRIVATE myalien-core)
add_test(NAME actions COMMAND actions_test)

add_executable(snapshot_test snapshot_test.cpp)
target_link_libraries(snapshot_test PRIVATE myalien-core)
add_test(NAME snapshot COMMAND snapshot_test)

# Service-side logic, compiled directly from src/svc (no service runtime involved).
add_executable(thermal_state_test thermal_state_test.cpp ${PROJECT_SOURCE_DIR}/src/svc/thermal_state.cpp)
target_include_directories(thermal_state_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(thermal_state_test PRIVATE myalien-core)
add_test(NAME thermal_state COMMAND thermal_state_test)
