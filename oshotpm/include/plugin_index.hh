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

#ifndef _PLUGIN_INDEX_HH_
#define _PLUGIN_INDEX_HH_

// The oshot plugin index (https://github.com/Toni500github/oshot-plugin-index):
// one index.toml listing plugins, their versions, and per version a pinned
// source and prebuilt files per target with their SHA-256.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "util.hpp"

// The repository name installed index plugins are grouped under in state.toml
constexpr char const INDEX_REPO_NAME[] = "index";  // NOLINT

struct index_artifact_t
{
    std::string url;
    std::string sha256;
};

struct index_source_t
{
    std::string git;
    std::string tag;
    std::string commit;
    std::string subdir;  // folder with the oshot-plugin.toml, empty for the root
};

struct index_version_t
{
    std::string version;
    std::string date;
    std::string min_oshot;
    std::string changes;
    int64_t     abi   = 0;
    int64_t     imgui = 0;

    std::optional<index_source_t>           source;
    std::map<std::string, index_artifact_t> artifacts;  // by target, e.g. "linux-x86_64"

    bool        yanked = false;
    std::string yanked_reason;
};

struct index_plugin_t
{
    std::string              id;
    std::string              name;
    std::string              description;
    std::string              homepage;
    std::string              license;
    std::vector<std::string> owners;
    std::vector<std::string> authors;
    std::vector<std::string> tags;
    bool                     network = false;  // the author says it connects to the internet

    std::vector<index_version_t> versions;  // newest first

    [[nodiscard]] const index_version_t* FindVersion(std::string_view version) const;
};

struct plugin_index_t
{
    std::string                 generated;
    std::vector<index_plugin_t> plugins;  // sorted by ID

    [[nodiscard]] const index_plugin_t* Find(std::string_view id) const;
};

// The index's URL, or $OSHOT_PLUGIN_INDEX (any URL curl takes, e.g. file://)
std::string plugin_index_url();

// Downloads the index to `cache_file` and parses it. With `allow_cached`, a
// failed download falls back to the last downloaded copy (and `stale` is set).
Result<plugin_index_t> fetch_plugin_index(const fs::path& cache_file, bool allow_cached, bool* stale = nullptr);

Result<plugin_index_t> parse_plugin_index(const std::string& path);

// Downloads `url` to `dest` with the curl program
Result<> download_file(const std::string& url, const fs::path& dest);

// Lowercase hex SHA-256 of a file
Result<std::string> sha256_file(const fs::path& path);

// <0, 0 or >0, like strcmp, for semantic versions ("1.10.0" > "1.9.0" > "1.9.0-beta")
int compare_versions(std::string_view a, std::string_view b);

// Whether this oshot can load the version (ABI, ImGui and oshot version) and
// install it on this platform (a prebuilt file for OSHOT_TARGET, or a source)
bool is_installable(const index_version_t& v, std::string* why_not = nullptr);

// The newest installable version that isn't yanked
const index_version_t* pick_version(const index_plugin_t& plugin, std::string* why_not = nullptr);

#endif
