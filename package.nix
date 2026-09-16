{ lib
, stdenv
, cmake
, extra-cmake-modules
, kwin
, wrapQtAppsHook
, qttools
}:

stdenv.mkDerivation rec {
  pname = "kwin-effect-xorcursor";
  version = "1.0";

  src = ./.;

  nativeBuildInputs = [
    cmake
    extra-cmake-modules
    wrapQtAppsHook
  ];

  buildInputs = with pkgs.kdePackages; [
    kwin            # <-- This provides kwineffects.h
    qtbase
  ];

  meta = with lib; {
    description = "XOR cursor effect for KDE Plasma";
    license = licenses.gpl2;
    homepage = "https://github.com/yonikek/kwin-effect-xorcursor";
  };
}
