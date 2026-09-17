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
          # Required if you depend on any unfree packages. Harmless otherwise.
          config.allowUnfree = true;
        };
      in
      {
        packages = {
          default = pkgs.callPackage ./package.nix { };
          xorcursor = pkgs.callPackage ./package.nix { };
        };

        # `nix develop` shell for working on the effect locally. Mirrors the
        # build inputs plus a few conveniences.
        devShells.default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.default ];
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
