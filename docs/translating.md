# Translating oshot

oshot's interface can be translated with standard gettext `.po` files: the overlay and all its windows, the tray
menu, notifications and dialog titles. Logs, command-line output and `oshotpm` stay in English, so bug reports stay
readable.

Each language is one file, `po/<code>.po`, built into oshot. The template with every string is
[`po/oshot.pot`](../po/oshot.pot).

## Picking the language

oshot follows the system language by default. To pick one, use Preferences > Defaults > Language, or set it in the
config file:

```toml
[default]
language = "it"   # or "auto" for the system language
```

The change applies the next time oshot starts. If oshot runs in the tray, quit it from the tray menu and start it
again.

`auto` reads the `LANGUAGE`, `LC_ALL`, `LC_MESSAGES` and `LANG` environment variables, then the Windows display
language or the macOS preferred languages. The closest catalog is used: `it_CH` gets `it`, and a regional catalog like
`pt_BR` is completed by `pt` when both exist. English is used when there's no catalog for the language.

## Adding or updating a translation

### Start a catalog

With GNU gettext installed:

```sh
scripts/update_translations.sh --init it
```

This creates `po/it.po` from the template. Without gettext, copy `po/oshot.pot` to `po/<code>.po` and fill in the
header: `Language: it`, `Content-Type: text/plain; charset=UTF-8` and the `Plural-Forms` line.

The file name is the language code: `it`, `ru`, `pt_BR`, `zh_CN`. oshot only picks codes that tinygettext knows (see
the table in [`src/libs/tinygettext/language.cpp`](../src/libs/tinygettext/language.cpp)), and logs a warning for any
other.

### Translate

Any `.po` editor works, for example [Poedit](https://poedit.net/), Lokalize or a text editor. Files must be UTF-8.

Keep these as they are in the English text:

| In the English text | Example | Rule |
| ------------------- | ------- | ---- |
| `{}` placeholders | `Installed {}` | Keep the same ones. To change their order, number them: `{1} … {0}`. |
| `%s`, `%d` placeholders | `Found in %s` | Keep the same ones, in the same order. |
| `\n` | `Delay before acquiring a screenshot (milliseconds).\nHas no effect…` | Line breaks are part of the text. |
| Key names and shortcuts | `CTRL+A`, `ESC` | Keep them as they're printed on keyboards. |
| Names | `oshot`, `Tesseract`, `.traineddata` | Don't translate. |

A translation whose placeholders don't match its English text is skipped when oshot loads the catalog (the English
text is shown instead) and logged as a warning, so a mistake can't crash oshot. `msgfmt --check` and
`scripts/update_translations.sh` report the `%` ones before that.

Entries marked fuzzy (Poedit's "Needs work") are never shown. Remove the mark once the translation is checked.

### Plural forms

A few strings depend on a count, such as `'{}' and its {} plugin` / `'{}' and its {} plugins`. Their `msgstr[0]`,
`msgstr[1]`, … follow the `Plural-Forms` header of the catalog. oshot reads plural rules with tinygettext, which only
knows the ones listed in [`src/libs/tinygettext/plural_forms.cpp`](../src/libs/tinygettext/plural_forms.cpp) (spaces
don't matter). With any other rule, plural strings stay in English. `scripts/update_translations.sh` warns about that.

For example:

| Language | Plural-Forms |
| -------- | ------------ |
| Italian, German, Spanish | `nplurals=2; plural=(n != 1);` |
| French, Brazilian Portuguese | `nplurals=2; plural=(n > 1);` |
| Russian, Ukrainian | `nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 \|\| n%100>=20) ? 1 : 2);` |
| Chinese, Japanese, Korean | `nplurals=1; plural=0;` |

### Test it without building oshot

Put the file in the `locale` folder of the config directory, then pick the language and restart oshot:

| System | Folder |
| ------ | ------ |
| Linux, BSD, macOS | `~/.config/oshot/locale/` (or `$XDG_CONFIG_HOME/oshot/locale/`) |
| Windows | `%APPDATA%\oshot\locale\` |

A catalog there replaces the built-in one with the same code, or adds a language. Tools > View Logs shows the
language oshot loaded (e.g. `Language: it (296 strings)`) and any warning about the catalog.

### Fonts

oshot draws its interface with the system UI font: the sans-serif font picked by fontconfig for the language on Linux
and BSD, Segoe UI on Windows, Helvetica on macOS. When it lacks the language's script, oshot adds a font that covers
it: the CJK fonts on Windows (Microsoft YaHei, Yu Gothic, Malgun Gothic, …), the Hiragino and Apple SD Gothic fonts and
Arial Unicode on macOS (for scripts past Latin, Greek and Cyrillic). Fonts in the Fonts section at the bottom
of Preferences > Defaults, or in the `fonts` option of the config, come first, and the system fonts fill in what they
lack. ImGui's built-in font, which only has Latin-1 characters, is used only when no system font is found, or when
"Use the built-in font" (`builtin-font` in the config) is on: the system fonts then only fill in the other characters.

If some characters still show as `?`, add a font with them to that section. Its font size (`font-size` in the
config) scales the whole interface. Buttons and windows are sized from the font, so longer translations usually fit,
but check the Preferences and the Text tools window after translating.

### Submit it

Open a pull request with the new or changed `po/<code>.po`. Text that doesn't fit its button or window is worth
mentioning there.

## For developers: adding strings

The functions are in [`include/i18n.hpp`](../include/i18n.hpp).

| Use | For |
| --- | --- |
| `_("Save")` | A translated string. The pointer stays valid, so it can go straight to ImGui. |
| `N_("Defaults")` | A string to translate later, e.g. in a static array; translate it where it's used with `i18n::tr(items[i])`. |
| `i18n::tr_fmt("Installed {}", version)` | A translated fmt format string. The English one is checked at compile time like `fmt::format`. |
| `i18n::tr_n_fmt("{} plugin", "{} plugins", n, n)` | A string that depends on a count. Both English forms must have the same placeholders. |

- ImGui IDs: in `_("Save##save_btn")` only `Save` is translated, and the ID stays the same. Give windows and popups a
  `###` ID (`_("Preferences###preferences_window")`), so their state doesn't depend on the language.
- Pass translated text to ImGui's printf-style functions (`Text`, `TextColored`, `SetTooltip`, …) as `"%s", _("…")`,
  or use `TextUnformatted`, unless it's a format string with arguments. Otherwise a `%` in a translation is read as a
  placeholder.
- Translate whole sentences. Don't build them from pieces: word order differs between languages.
- Leave a note for translators with a comment starting with `TRANSLATORS:` on the line before the string. Mark a
  string with `%` that isn't a printf format with `// xgettext:no-c-format`.
- Log messages (`spdlog`), command-line output and `oshotpm` stay in English. When a message is both shown and logged,
  translate only the shown copy (see `fail()` in `src/hotkey.cpp`).

After changing strings, update the template and the catalogs:

```sh
scripts/update_translations.sh          # regenerates po/oshot.pot and merges it into po/*.po
scripts/update_translations.sh --check  # fails if po/oshot.pot is out of date
```

The script needs GNU gettext (xgettext, msguniq, msgmerge, msginit, msgfmt). Commit `po/oshot.pot` and the changed
catalogs with the code. A new `po/<code>.po` is built into oshot without any CMake change.
