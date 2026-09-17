{ lib
, stdenv
, cmake
, ninja
, pkg-config

# KDE Frameworks 6 and Qt 6 — everything KDE-related comes from the
# `kdePackages` scope. Top-level aliases (kwin, kcmutils, ki18n,
# extra-cmake-modules, ...) have been removed from nixpkgs since KDE Gear 5
# and Plasma 5 reached end of life.
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
    kdePackages.extra-cmake-modules        # ECM CMake modules
    qt6.qttools                            # uic, invoked by ki18n_wrap_ui()
    kdePackages.kconfig                    # kconfig_compiler
  ];

  buildInputs = [
    kdePackages.kwin                       # KWin::kwin
    kdePackages.kcmutils                   # KCModule, KConfigDialogManager
    kdePackages.ki18n                      # ki18n_wrap_ui(), KF6::I18n
    kdePackages.kconfig                    # KF6::ConfigCore
    kdePackages.kcoreaddons                # KF6::CoreAddons
    qt6.qtbase                             # Qt6::DBus, QDBusConnection
  ];

  # This derivation produces two Qt plugin modules (.so files loaded at
  # runtime by KWin), not executables. There is no bin/ directory for
  # wrapQtAppsHook to operate on, so opt out of the qtPreHook safety check.
  dontWrapQtApps = true;

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DKDE_INSTALL_USE_QT_SYS_PATHS=ON"
    "-DCMAKE_MODULE_PATH=${kdePackages.kwin}/lib/cmake/kwin"
  ];

  doCheck = false;

  postInstall = ''
    if [ -d "$out/plugins/kwin" ] && [ ! -e "$out/lib/plugins/kwin" ]; then
      mkdir -p "$out/lib/plugins"
      mv "$out/plugins/kwin" "$out/lib/plugins/kwin"
      rmdir "$out/plugins" 2>/dev/null || true
    fi

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
