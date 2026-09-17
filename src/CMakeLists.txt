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

# Locate the D-Bus interface description for org.kde.kwin.Effects. KWin
# installs it under share/dbus-1/interfaces/, but the exact prefix depends
# on KDE_INSTALL_DATADIR. find_file() searches all of the standard install
# locations including the KWin dev output on Nix.
find_file(KWIN_EFFECTS_DBUS_XML
    NAMES org.kde.kwin.Effects.xml
    PATHS
        ${KDE_INSTALL_DATADIR}/dbus-1/interfaces
        ${CMAKE_INSTALL_PREFIX}/share/dbus-1/interfaces
        /usr/share/dbus-1/interfaces
    NO_DEFAULT_PATH
)

if(KWIN_EFFECTS_DBUS_XML)
    qt_add_dbus_interface(xorcursor_config_SRCS
        ${KWIN_EFFECTS_DBUS_XML}
        kwineffects_interface
    )
    target_compile_definitions(kwin_xorcursor_config PRIVATE
        HAVE_KWIN_EFFECTS_DBUS=1
    )
else()
    message(WARNING
        "org.kde.kwin.Effects.xml not found; the config module will not "
        "be able to trigger an immediate effect reconfigure. Changes will "
        "apply on the next KWin restart or effect toggle.")
endif()

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
