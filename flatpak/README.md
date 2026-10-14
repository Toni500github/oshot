# Flatpak

`io.github.toni500github.oshot.yml` builds the local checkout as a Flatpak,
without plugin support, against the GNOME runtime. Plugins are left out
because they are built from git with bash and a compiler, which the sandbox
doesn't have.

The Flatpak published on Flathub is built from its own manifest in
https://github.com/flathub/io.github.toni500github.oshot, which builds release
tags. Send changes to the Flathub package there.

## Build and install

From the repository root:

```bash
flatpak run org.flatpak.Builder --user --install --force-clean \
  --state-dir=build/flatpak-state build/flatpak flatpak/io.github.toni500github.oshot.yml
flatpak run io.github.toni500github.oshot
```

## Check a build the way Flathub does

```bash
flatpak run org.flatpak.Builder --force-clean --sandbox --user --install \
  --install-deps-from=flathub --ccache --mirror-screenshots-url=https://dl.flathub.org/media/ \
  --state-dir=build/flatpak-state --repo=build/flatpak-repo build/flatpak flatpak/io.github.toni500github.oshot.yml
flatpak run --command=flatpak-builder-lint org.flatpak.Builder manifest flatpak/io.github.toni500github.oshot.yml
flatpak run --command=flatpak-builder-lint org.flatpak.Builder repo build/flatpak-repo
```

`--mirror-screenshots-url` downloads the screenshots listed in
`assets/linux/io.github.toni500github.oshot.metainfo.xml`, so the build fails
if one of those URLs is unreachable.
