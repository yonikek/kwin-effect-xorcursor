# ---------------------------------------------------------------- effect
kcoreaddons_add_plugin(xorcursor
    INSTALL_NAMESPACE "kwin/effects/plugins/"
)

target_sources(xorcursor PRIVATE
    xorcursor.cpp
    main.cpp
)

target_link_libraries(xorcursor
    KF6::CoreAddons
    KWin::kwin
)

# ------------------------------------------------------------ config KCM
set(xorcursor_config_SRCS
    xorcursor_config.cpp
    xorcursor_config.h
)

ki18n_wrap_ui(xorcursor_config_SRCS xorcursor_config.ui)
kconfig_add_kcfg_files(xorcursor_config_SRCS xorcursorconfig.kcfgc)

qt_add_dbus_interface(xorcursor_config_SRCS
    ${KWIN_EFFECTS_DBUS_XML}
    kwineffects_interface
)

add_library(kwin_xorcursor_config MODULE ${xorcursor_config_SRCS})

target_link_libraries(kwin_xorcursor_config
    KF6::KCMUtils
    KF6::CoreAddons
    KF6::I18n
    Qt6::DBus
)

install(TARGETS kwin_xorcursor_config
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/configs
)
