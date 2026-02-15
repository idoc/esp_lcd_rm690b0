{
  description = "Dev env for ESP IDF, with SDL2 for desktop LVGL";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=8a6d5427d99ec71c64f0b93d45778c889005d9c2";
  };

  outputs = {nixpkgs, ...}: let
    nixpkgs-esp-dev = fetchGit {
      url = "https://github.com/mirrexagon/nixpkgs-esp-dev.git";

      # Optionally pin to a specific commit of `nixpkgs-esp-dev`.
      rev = "84edf59e2059dbed15e534a3c21974e7d3e32b30";
    };

    pkgs = import nixpkgs {
      system = "aarch64-darwin";

      overlays = [(import "${nixpkgs-esp-dev}/overlay.nix")];

      # The Python library ecdsa is marked as insecure, but we need it for esptool.
      # See https://github.com/mirrexagon/nixpkgs-esp-dev/issues/109
      config.permittedInsecurePackages = [
        "python3.13-ecdsa-0.19.1"
      ];
    };
  in {
    devShells."aarch64-darwin".default = pkgs.mkShell {
      name = "sdl2-project";

      buildInputs = with pkgs; [
        SDL2.dev
        apple-sdk_15
      ];
    };
  };
}
