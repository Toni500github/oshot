{
  description = "oshot; a program to screenshot and get text from images";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs, ... }:
    let 
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      version = "0.5.0-rc1";

      mkOshot = { disablePlugins ? false }:
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
            
            systemd
            libarchive
            curl
            sysprof
          ];

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DDISABLE_PLUGINS=${if disablePlugins then "ON" else "OFF"}"
          ];

          installPhase = ''
            cmake --install . --prefix "$out"

            # Write tesseract path to file
            printf 'ocr-path = "${pkgs.tesseract}/bin/tesseract"' > "$out/oshot-config.toml"
          '';
        };
    in {
      packages.${system} = {
        oshot-plugins = mkOshot {
          disablePlugins = false;
        };

        oshot = mkOshot {
          disablePlugins = true;
        };

        default = self.packages.${system}.oshot;
      };
      devShells.${system}.default = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
          cmake
          ninja
          pkg-config
          wrapGAppsHook3
          wayland-scanner
          gdb
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

          systemd
          libarchive
          curl
          sysprof
        ];
      };
    };
}
