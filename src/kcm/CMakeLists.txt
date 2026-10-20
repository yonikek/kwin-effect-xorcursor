# Config KCM for the xorcursor effect. Shown when the user clicks the configure
# button next to the effect in the Desktop Effects module.
set(kcm_xorcursor_SRCS
    xorcursor_config.cpp
    xorcursor_config.h
)

# The KCM gets its own copy of the generated settings class, exactly like the
# effect itself does; both live in separate shared objects.
kconfig_add_kcfg_files(kcm_xorcursor_SRCS ../xorcursorsettings.kcfgc)

ki18n_wrap_ui(kcm_xorcursor_SRCS xorcursor_config.ui)

# OrgKdeKwinEffectsInterface, to tell the compositor to reread the options.
qt_add_dbus_interface(kcm_xorcursor_SRCS ${KWIN_EFFECTS_INTERFACE} kwineffects_interface)

kcoreaddons_add_plugin(kcm_kwineffects_xorcursor INSTALL_NAMESPACE "kwin/effects/configs/")

target_sources(kcm_kwineffects_xorcursor PRIVATE
    ${kcm_xorcursor_SRCS}
)

target_link_libraries(kcm_kwineffects_xorcursor
    KF6::CoreAddons
    KF6::KCMUtils
    KF6::I18n
    KF6::ConfigCore
    KF6::ConfigGui
    Qt6::Widgets
    Qt6::DBus
)
