kcoreaddons_add_plugin(xorcursor INSTALL_NAMESPACE "kwin/effects/plugins/")

qt_add_resources(xorcursor "invertcursor_shaders"
    PREFIX "/effects/cursor_invert"
    FILES shaders/cursor_invert.frag
)

target_sources(xorcursor PRIVATE
    xorcursor.cpp
    main.cpp
)

target_link_libraries(xorcursor
    KF6::CoreAddons
    KWin::kwin
)
