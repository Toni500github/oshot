{
  description = "Nix flake for oshot; a simple and lightweight tool for extracting text from a screenshot/image (on the fly)";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";
  };

  outputs = { self, nixpkgs }: {
    packages.x86_64-linux.oshot =
      with import nixpkgs { system = "x86_64-linux"; };
      stdenv.mkDerivation {
        name = "oshot";
        src = self;
        nativeBuildInputs = [ cmake gnumake pkg-config git ];
        buildInputs = [ glfw3 leptonica libx11.dev tesseract zbar.dev libappindicator-gtk3.dev dbus.dev systemd.dev libsysprof-capture pcre2.dev libxdmcp.dev libuuid.dev libselinux.dev libsepol.dev libthai.dev libdatrie.dev libdeflate lerc.dev xz.dev zstd.dev libwebp libxkbcommon.dev libepoxy.dev libxtst giflib ];
        configurePhase = ''
          cmake -DCMAKE_BUILD_PREFIX=/usr -DDEBUG=0 -G "Unix Makefiles" -B build -S .
        '';
        buildPhase = ''
          cmake --build build -j$(nproc)
        '';
        installPhase = ''
          install -Dm755 build/oshot $out/bin/oshot
          install -Dm644 oshot.desktop $out/share/applications/oshot.desktop
          install -Dm644 LICENSE $out/share/licenses/oshot/LICENSE
        '';
      };
    packages.x86_64-linux.default = self.packages.x86_64-linux.oshot;
  };
}
