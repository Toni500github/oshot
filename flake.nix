{
  description = "oshot, the screenshot tool for extracting text and QR codes on the fly";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      version = "0.5.0-rc2";

      # A left click on the tray icon needs the "activate" signal of Ayatana AppIndicator 0.6+.
      # Until nixpkgs has it, 0.6.1 is built from upstream's source
      ayatanaAppindicator = pkgs:
        if pkgs.lib.versionAtLeast pkgs.libayatana-appindicator.version "0.6" then
          pkgs.libayatana-appindicator
        else
          pkgs.libayatana-appindicator.overrideAttrs (old: {
            version = "0.6.1";
            src = pkgs.fetchFromGitHub {
              owner = "AyatanaIndicators";
              repo = "libayatana-appindicator";
              rev = "0.6.1";
              hash = "sha256-JMrOngNvNoeIoIznusvt56Cd2eI7o0r2CdT5Q8A7Pfc=";
            };
            # The gtk-doc scanner fails to link, and nothing needs the docs
            cmakeFlags = (old.cmakeFlags or [ ]) ++ [ "-DENABLE_GTKDOC=OFF" ];
          });

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
            sdl3
            tesseract
            leptonica
            zbar
            zlib
            libGL
            libpng

            libx11
            libxcb
            libxrandr
            glib
            gtk3
            fontconfig
            (ayatanaAppindicator pkgs)
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

          # oshot runs these: xclip/wl-copy to copy, zenity for the file dialogs
          preFixup = ''
            gappsWrapperArgs+=(--prefix PATH : ${pkgs.lib.makeBinPath (with pkgs; [ xclip wl-clipboard zenity ])})
          '';

          meta = with pkgs.lib; {
            description = "Screenshot tool for extracting text and QR codes on the fly";
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

      # nix develop: the plugin build's dependencies, a debugger and the tools oshot runs
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ (mkOshot pkgs { }) ];
          packages = with pkgs; [ gdb xclip wl-clipboard zenity ];
        };
      });
    };
}
