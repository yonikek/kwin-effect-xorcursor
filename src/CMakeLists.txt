set(xorcursor_SOURCES
    xorcursor.cpp
    main.cpp
)

set(xorcursor_SETTINGS_SOURCES)
kconfig_add_kcfg_files(xorcursor_SETTINGS_SOURCES xorcursorsettings.kcfgc)

kcoreaddons_add_plugin(xorcursor INSTALL_NAMESPACE "kwin/effects/plugins/")

target_sources(xorcursor PRIVATE
    ${xorcursor_SOURCES}
    ${xorcursor_SETTINGS_SOURCES}
)

target_link_libraries(xorcursor
    KF6::CoreAddons
    KF6::ConfigCore
    KF6::ConfigGui
    KWin::kwin
)

add_subdirectory(kcm)
