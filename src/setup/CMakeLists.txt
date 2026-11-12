# myalien-setup.exe: carries the three programs as resources (res/setup.rc).
add_executable(myalien-setup WIN32 main.cpp ${PROJECT_SOURCE_DIR}/res/setup.rc)
target_link_libraries(myalien-setup PRIVATE comctl32 shell32 ole32 oleaut32 advapi32 uuid)
add_dependencies(myalien-setup myalien myalien-svc myalien-tray)

# rc.exe finds the payload files through the include path.
target_include_directories(myalien-setup PRIVATE
    $<TARGET_FILE_DIR:myalien> $<TARGET_FILE_DIR:myalien-svc> $<TARGET_FILE_DIR:myalien-tray>)

# Installing needs administrator rights: Windows asks for them when setup starts.
target_link_options(myalien-setup PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
