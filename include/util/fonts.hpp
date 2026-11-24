#include "util/paths.hpp"
/*
 * Copyright 2026 Toni500
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _UTIL_FONTS_HPP_
#  define _UTIL_FONTS_HPP_

// Font files and ImGui's font atlas

#  include <filesystem>
#  include <string>
#  include <vector>

struct ImFont;
struct ImFontAtlas;
struct ImFontConfig;
struct ImGuiIO;

// ------------------------------
// Functions
// ------------------------------

/**
 * Find a font file by path or by file name in the system and user font directories
 * @param font An existing path, or a file name like "DejaVuSans.ttf"
 * @return The font's path, or empty if not found
 **/
fs::path get_font_path(const std::string& font);

/**
 * List the TrueType and OpenType fonts in the system and user font directories
 * @return One path per file name, the one get_font_path() finds for that name
 **/
std::vector<fs::path> list_font_files();

/**
 * Rebuild ImGui's font atlas from the fonts option, then ImGui's own font with builtin-font,
 * then the system UI font and the fonts covering the UI language's script. The first font found
 * is the default one and the others fill in its missing glyphs. Falls back to ImGui's own font
 * when none is found
 * @param io The ImGui IO whose atlas to rebuild
 **/
void build_font_atlas(ImGuiIO& io);

/**
 * ImFontAtlas::AddFontFromFileTTF() that returns nullptr for a file it can't load,
 * where ImGui would assert in a debug build
 * @param atlas The atlas to add the font to
 * @param path The font file
 * @param size The font's reference size in pixels
 * @param cfg Options for the font, e.g. MergeMode, or nullptr for the defaults
 * @return The font, or nullptr
 **/
ImFont* add_font_file(ImFontAtlas* atlas, const fs::path& path, float size, const ImFontConfig* cfg = nullptr);

#endif  // _UTIL_FONTS_HPP_
