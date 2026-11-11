add_executable(myalien-tray WIN32 main.cpp tray_app.cpp worker.cpp icon.cpp autostart.cpp)
target_link_libraries(myalien-tray PRIVATE myalien-core user32 gdi32 shell32 advapi32)
