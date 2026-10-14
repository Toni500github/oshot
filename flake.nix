{
  description = "oshot; a program to screenshot and get text from images";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      version = "0.5.0-rc2";

      mkOshot = pkgs: { disablePlugins ? false }:
        pkgs.stdenv.mkDerivation {
          pname = if disablePlugins then "oshot" else "oshot-plugins";
          inherit version;

          src = ./.;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            wrapGAppsHook3
            wayland-scanner
          ];

          buildInputs = with pkgs; [
            glfw3
            tesseract
            leptonica
            zbar
            libGL
            libpng

            libx11
            libxcb
            libxrandr
            glib
            gtk3
            libappindicator-gtk3
            dbus
            wayland
            wayland-protocols

            systemd
            libarchive
            curl
            libsysprof-capture
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DDISABLE_PLUGINS=${if disablePlugins then "ON" else "OFF"}"
          ];

          installPhase = ''
            cmake --install . --prefix "$out"
          '';

          meta = with pkgs.lib; {
            description = "Screenshot tool to extract text, scan QR codes and barcodes on the fly";
            homepage = "https://github.com/Toni500github/oshot";
            license = licenses.bsd3;
            mainProgram = "oshot";
            platforms = platforms.linux;
          };
        };
    in {
      packages = forAllSystems (pkgs: {
        oshot-plugins = mkOshot pkgs {
          disablePlugins = false;
        };

        oshot = mkOshot pkgs {
          disablePlugins = true;
        };

        default = self.packages.${pkgs.stdenv.hostPlatform.system}.oshot;
      });
    };
}
