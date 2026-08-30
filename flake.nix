{
  description = "Nix flake for oshot; a simple and lightweight tool for extracting text from a screenshot/image (on the fly)";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
      nixpkgsFor = forAllSystems (system: import nixpkgs { inherit system; });
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgsFor.${system};
        in
        {
          oshot = pkgs.stdenv.mkDerivation {
            pname = "oshot";
            version = "0.5.0-rc1";

            src = ./.;

            nativeBuildInputs = with pkgs; [
              cmake
              gnumake
              pkg-config
              git
              wrapGAppsHook3
              wayland-scanner
            ];

            buildInputs = with pkgs; [
              glfw3
              leptonica
              tesseract
              zbar
              gtk3
              libappindicator-gtk3
              dbus
              systemd
              libX11
              libXtst
              giflib
              libwebp
              libsysprof-capture
              libXdmcp
              zenity
              pcre2
            ];

            # Initialize git AND add a tag so `git describe` works
            preConfigure = ''
              export SOURCE_DATE_EPOCH=1
              export GIT_AUTHOR_DATE="1970-01-01T00:00:01Z"
              export GIT_COMMENTOR_DATE="1970-01-01T00:00:01Z"

              git init -b nix-build
              git config user.name "Nix"
              git config user.email "nix@localhost"
              git add .
              git commit -m "dummy commit for build"
              git tag -a v0.1.0 -m "v0.1.0"
            '';

            installPhase = ''
              runHook preInstall

              install -Dm755 oshot $out/bin/oshot
              if [ -f oshot.desktop ]; then
                install -Dm644 oshot.desktop $out/share/applications/oshot.desktop
              fi
              if [ -f LICENSE ]; then
                install -Dm644 LICENSE $out/share/licenses/oshot/LICENSE
              fi

              runHook postInstall
            '';
          };

          default = self.packages.${system}.oshot;
        });

      nixosModules.default = { config, lib, pkgs, ... }: {
        options.programs.oshot.enable = lib.mkEnableOption "oshot text extraction and screenshot tool";

        config = lib.mkIf config.programs.oshot.enable {
          environment.systemPackages = [ 
            self.packages.${pkgs.system}.oshot
          ];  
        };
      };
    };
}
