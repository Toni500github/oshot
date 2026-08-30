{
  description = "Nix flake for oshot; a simple and lightweight tool for extracting text from a screenshot/image (on the fly)";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";

    oshot-src = {
      url = "github:Toni500github/oshot";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, oshot-src }:
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

            src = oshot-src;

            nativeBuildInputs = with pkgs; [
              cmake
              gnumake
              pkg-config
              git
              wrapGAppsHook3
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
            ];

            # Initialize git AND add a tag so `git describe` works
            preConfigure = ''
              git init
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
    };
}
