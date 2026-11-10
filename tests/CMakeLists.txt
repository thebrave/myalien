add_executable(charge_plan_test charge_plan_test.cpp)
target_link_libraries(charge_plan_test PRIVATE myalien-core)
add_test(NAME charge_plan COMMAND charge_plan_test)
