# Flatpak

`io.github.toni500github.oshot.yml` builds oshot without plugin support
against the GNOME runtime. It builds the local checkout, for testing:

```bash
flatpak run org.flatpak.Builder --user --install --force-clean \
  --state-dir=build/flatpak-state build/flatpak flatpak/io.github.toni500github.oshot.yml
```

## Flathub

The Flathub copy builds a release tag instead of the checkout:

1. Add a `<release>` for the version to
   `assets/linux/io.github.toni500github.oshot.metainfo.xml`, then tag it.
   Flathub reads the metainfo of the tag.
2. `./scripts/flathub_manifest.sh vX.Y.Z` writes the manifest and its
   patches to `build/flathub/`.
3. First submission: open a PR with those files against the `new-pr`
   branch of https://github.com/flathub/flathub (see
   https://docs.flathub.org/docs/for-app-authors/submission).
   Afterwards they live in https://github.com/flathub/io.github.toni500github.oshot,
   where Flathub's bot opens a PR for every new stable `vX.Y.Z` tag.

When a dependency or the runtime changes here, copy the change there too.

Check a build the way Flathub does:

```bash
flatpak run org.flatpak.Builder --force-clean --sandbox --user --install \
  --install-deps-from=flathub --ccache --mirror-screenshots-url=https://dl.flathub.org/media/ \
  --state-dir=build/flatpak-state --repo=build/flatpak-repo build/flatpak flatpak/io.github.toni500github.oshot.yml
flatpak run --command=flatpak-builder-lint org.flatpak.Builder manifest flatpak/io.github.toni500github.oshot.yml
flatpak run --command=flatpak-builder-lint org.flatpak.Builder repo build/flatpak-repo
```

The screenshots in the metainfo are fetched from the `main` branch, so
`--mirror-screenshots-url` only works once `assets/screenshots/` is there.
