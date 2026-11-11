add_executable(myalien-tray WIN32 main.cpp tray_app.cpp status_window.cpp requests.cpp worker.cpp icon.cpp autostart.cpp
    game_mode.cpp game_settings.cpp game_watch.cpp games_dialog.cpp
    ${PROJECT_SOURCE_DIR}/res/tray.rc)
target_link_libraries(myalien-tray PRIVATE myalien-core user32 gdi32 shell32 advapi32 comctl32)
