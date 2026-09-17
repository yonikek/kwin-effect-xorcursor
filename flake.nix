{
  description = "KWin XOR cursor effect";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
        };

        # Single derivation, shared between both package attrs. Building
        # this once and aliasing avoids a duplicate build.
        xorcursor = pkgs.callPackage ./package.nix { };
      in
      {
        packages = {
          inherit xorcursor;
          default = xorcursor;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ xorcursor ];
          packages = with pkgs; [
            cmake
            ninja
            pkg-config
            clang-tools
            kdePackages.kconfig
          ];
        };
      });
}
