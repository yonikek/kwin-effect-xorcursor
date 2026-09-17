# ---------------------------------------------------------------- KF6 deps
#
# The top-level CMakeLists.txt finds KF6CoreAddons (needed by
# kcoreaddons_add_plugin for the effect target). The config KCM additionally
# needs I18n (ki18n_wrap_ui), Config (kconfig_add_kcfg_files), and KCMUtils
# (KCModule).
find_package(KF6 REQUIRED COMPONENTS
    CoreAddons
    Config
    I18n
    KCMUtils
)

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

add_library(kwin_xorcursor_config MODULE ${xorcursor_config_SRCS})

target_link_libraries(kwin_xorcursor_config
    KF6::KCMUtils
    KF6::CoreAddons
    KF6::I18n
)

install(TARGETS kwin_xorcursor_config
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/configs
)
