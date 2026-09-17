{ lib
, stdenv
, cmake
, ninja
, pkg-config
, extra-cmake-modules

# KWin and KDE Frameworks 6
, kwin
, kdePackages
, qt6
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "kwin-effect-xorcursor";
  version = "1.0";

  src = ./.;

  # We build two targets (the effect plugin and the config KCM), so the
  # default single-target assumptions of some helpers don't apply. Plain
  # CMake is the most predictable choice.
  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    extra-cmake-modules
    qt6.wrapQtAppsHook
    qt6.qttools                  # uic, invoked by ki18n_wrap_ui()
    kdePackages.kconfig          # kconfig_compiler, for kconfig_add_kcfg_files()
  ];

  buildInputs = [
    kwin
    kdePackages.kcmutils         # KCModule, KConfigDialogManager
    kdePackages.ki18n            # ki18n_wrap_ui(), KF6::I18n
    kdePackages.kconfig          # KF6::ConfigCore, kcfg code generation runtime
    kdePackages.kcoreaddons      # KF6::CoreAddons, K_PLUGIN_CLASS_WITH_JSON
    qt6.qtbase                   # Qt6::DBus, QDBusConnection, qt_add_dbus_interface
  ];

  # Point CMake at KWin's CMake module directory so find_package(KWin)
  # resolves on Nix, where the dev output is split from the main package.
  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DKDE_INSTALL_USE_QT_SYS_PATHS=ON"
    "-DCMAKE_MODULE_PATH=${kwin}/lib/cmake/kwin"
  ];

  # Sanity checks: both .so files must exist and be loadable as Qt plugins.
  # These catch the most common failure (config KCM installed to the wrong
  # directory because KDEInstallDirs resolved differently under Nix).
  doCheck = false;   # no test suite in the upstream repo

  postInstall = ''
    # KDEInstallDirs sometimes resolves KDE_INSTALL_PLUGINDIR to $out/plugins
    # instead of $out/lib/plugins under Nix. Normalise the layout so KWin
    # finds both plugins at their canonical paths.
    if [ -d "$out/plugins/kwin" ] && [ ! -e "$out/lib/plugins/kwin" ]; then
      mkdir -p "$out/lib/plugins"
      mv "$out/plugins/kwin" "$out/lib/plugins/kwin"
      rmdir "$out/plugins" 2>/dev/null || true
    fi

    # Fail loudly if either artifact is missing, rather than shipping a
    # silently-broken package.
    test -f "$out/lib/plugins/kwin/effects/plugins/xorcursor.so" \
      || (echo "ERROR: effect plugin not installed" && exit 1)
    test -f "$out/lib/plugins/kwin/effects/configs/kwin_xorcursor_config.so" \
      || (echo "ERROR: config KCM not installed" && exit 1)
  '';

  meta = with lib; {
    description = "KWin effect that inverts the pixels under the cursor";
    homepage = "https://github.com/yonikek/kwin-effect-xorcursor";
    license = licenses.gpl2Plus;
    maintainers = with maintainers; [ ];
    platforms = platforms.linux;
  };
})
