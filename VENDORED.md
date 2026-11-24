# Vendored libraries

The upstream commit each library in `include/libs/` and `src/libs/` was last pulled from.
Update the commit and date here whenever a library is updated, and re-apply the local patches.

| Library | Upstream commit | Date | Local patches |
|---|---|---|---|
| Dear ImGui | https://github.com/ocornut/imgui/commit/77fe4588e6af934b4167b91312174ca3fbb0d71a | 2026-11-16 | `imconfig.h`: demo windows and debug tools disabled |
| cimgui | https://github.com/cimgui/cimgui/commit/2a6a93fccd10f275ee9206167dc9698f6f00fac1 | 2026-10-18 | Regenerated against the ImGui commit above (master, not docking) with oshot's `imconfig.h`; the demo and debug functions libimgui doesn't define are in the generator's `cimgui_skipped` |
| {fmt} | https://github.com/fmtlib/fmt/commit/14ac0d5a9d40aef14f50954069c1e10971abb0b2 | 2026-11-16 | |
| spdlog | https://github.com/gabime/spdlog/commit/6d9805df3f588402b17073dcd13eb569ceda864d | 2026-10-24 | `tweakme.h`: `SPDLOG_FMT_EXTERNAL`; `fmt/bundled/` removed |
| toml++ | https://github.com/marzer/tomlplusplus/commit/c38c9b922a68fc9772b25d7f606133d3ec4decfd | 2026-11-15 | `toml.hpp` renamed to `toml++.hpp` |
| clip | https://github.com/dacap/clip/commit/9cd472bbd8a97b115957fc5220f34e78fbac825e | 2026-10-19 | `clip_x11.cpp` removed; `clip_none.cpp`: unused parameter names removed |
| nvdialog | https://github.com/tseli0s/nvdialog/commit/db266b2f2b8f93119852830c4b9b5c36b750251c | 2026-11-16 | Public `nvd_set_error()`; `NVD_FILE_NAME` in `nvdialog_assert.h`; cocoa dialog, file dialog and notification fixes; win32 notification `NIM_MODIFY` fallback |
| tinygettext | https://github.com/tinygettext/tinygettext/commit/0ebef71ef7aa07ec6d026164fbe9472e6f82a583 | 2026-08-10 | `use_fuzzy` passed on to `POParser::parse()`; headers moved up one folder |
| tiny-process-library | https://gitlab.com/eidheim/tiny-process-library/-/commit/8bbb5a211c5c9df8ee69301da9d22fb977b27dc1 | 2025-01-07 | `process_unix.cpp`: `_exit()` in the child, Android `_CS_PATH` fallback |
| stb_image, stb_image_write | https://github.com/nothings/stb/commit/9600522de53973336fa033a289858a4354cbb04c | 2026-11-12 | |
| utf8.h | https://github.com/sheredom/utf8.h/commit/913014e0d56051b242a1959d24d3f3cb5d13a9ef | 2026-11-10 | |
| dotenv | https://github.com/harshfeudal/dotenv/commit/b353e13d559cc467f86d59005669cb36e797b91d | 2025-03-06 | `data()` accessor, `parse_line()` public |
| getopt_port | https://github.com/kimgr/getopt_port/commit/9d3d387087d252970923db7f297f681622c4e026 | 2024-03-03 | `getopt.h` renamed to `getopt_port.h`; `no_argument`, `required_argument`, `optional_argument` are 0, 1, 2 |
| sha256 | https://github.com/B-Con/crypto-algorithms/commit/cfbde48414baacf51fc7c74f275190881f037d32 | 2015-04-26 | `WORD` renamed to `_WORD` (clashes with `<windows.h>`) |
