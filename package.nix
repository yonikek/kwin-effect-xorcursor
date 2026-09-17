{ lib
, stdenv
, cmake
, ninja
, pkg-config
, extra-cmake-modules
, wrapQtAppsHook

# KWin and KDE Frameworks 6
, kwin
, kdePackages
, qt6
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "kwin-effect-xorcursor";
  version = "1.0";

  src = ./.;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    extra-cmake-modules
    wrapQtAppsHook
    qt6.qttools                  # uic, invoked by ki18n_wrap_ui()
    kdePackages.kconfig          # kconfig_compiler
  ];

  buildInputs = [
    kwin
    kdePackages.kcmutils         # KCModule, KConfigDialogManager
    kdePackages.ki18n            # ki18n_wrap_ui(), KF6::I18n
    kdePackages.kconfig          # KF6::ConfigCore
    kdePackages.kcoreaddons      # KF6::CoreAddons, kcoreaddons_add_plugin
    qt6.qtbase                   # Qt6::DBus, QDBusConnection
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DKDE_INSTALL_USE_QT_SYS_PATHS=ON"
    "-DCMAKE_MODULE_PATH=${kwin}/lib/cmake/kwin"
  ];

  doCheck = false;

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
    # silently-broken package. If the effect path differs, adjust here.
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
