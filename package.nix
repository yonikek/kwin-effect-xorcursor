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

  # The install path is lib/qt-6/plugins/kwin/... with
  # KDE_INSTALL_USE_QT_SYS_PATHS=ON on KF6, and lib/plugins/kwin/... with
  # it off. Rather than hard-code either, locate the files with find() so
  # the checks survive layout changes across KF6 versions.
  postInstall = ''
    effect_so=$(find "$out" -name 'xorcursor.so'          -print -quit)
    config_so=$(find "$out" -name 'kwin_xorcursor_config.so' -print -quit)

    if [ -z "$effect_so" ]; then
      echo "ERROR: effect plugin not installed" >&2
      exit 1
    fi
    if [ -z "$config_so" ]; then
      echo "ERROR: config KCM not installed" >&2
      exit 1
    fi
  '';

  meta = with lib; {
    description = "KWin effect that inverts the pixels under the cursor";
    homepage = "https://github.com/yonikek/kwin-effect-xorcursor";
    license = licenses.gpl2Plus;
    maintainers = with maintainers; [ ];
    platforms = platforms.linux;
  };
})
