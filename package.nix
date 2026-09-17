{ lib
, stdenv
, cmake
, ninja
, pkg-config

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
    kdePackages.extra-cmake-modules
    qt6.qttools
    kdePackages.kconfig
  ];

  buildInputs = [
    kdePackages.kwin
    kdePackages.kcmutils
    kdePackages.ki18n
    kdePackages.kconfig
    kdePackages.kcoreaddons
    qt6.qtbase
  ];

  # Plugin modules, not executables — no bin/ directory to wrap.
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
