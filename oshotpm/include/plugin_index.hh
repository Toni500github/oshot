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

// A prebuilt file of a version, either a bare library or an archive
struct index_artifact_t
{
    std::string url;
    std::string sha256;
};

// Where to build a version from. Built with designated initializers, so keep the field order
struct index_source_t
{
    std::string git;
    std::string tag;
    std::string commit;
    std::string subdir;  // folder with the oshot-plugin.toml, empty for the root
};

struct index_version_t
{
    // By target, e.g. "linux-x86_64"
    std::map<std::string, index_artifact_t> artifacts;

    // The plugin ABI and IMGUI_VERSION_NUM it's built for
    int64_t abi   = 0;
    int64_t imgui = 0;

    // The oldest oshot version it works with
    std::string min_oshot;

    // Withdrawn versions are skipped by pick_version(), and updating warns if it's the installed one
    bool        yanked = false;
    std::string yanked_reason;

    std::string                   version;
    std::string                   date;
    std::string                   changes;
    std::optional<index_source_t> source;
};

struct index_plugin_t
{
    // What the author says it does on its own and why (capability -> reason), as in a manifest
    std::map<std::string, std::string> capabilities;

    // newest first
    std::vector<index_version_t> versions;

    std::string              id;
    std::string              name;
    std::string              description;
    std::string              homepage;
    std::string              license;
    std::vector<std::string> owners;
    std::vector<std::string> authors;
    std::vector<std::string> tags;

    [[nodiscard]] const index_version_t* FindVersion(const std::string_view version) const;
};

struct plugin_index_t
{
    // sorted by ID
    std::vector<index_plugin_t> plugins;

    std::string generated;

    [[nodiscard]] const index_plugin_t* Find(const std::string_view id) const;
};

// The repository name installed index plugins are grouped under in state.toml
constexpr char const INDEX_REPO_NAME[] = "index";  // NOLINT

// ------------------------------
// The index
// ------------------------------

/**
 * Get the index's URL
 * @return $OSHOT_PLUGIN_INDEX if set (any URL curl takes, e.g. file://), else the official one
 **/
std::string plugin_index_url();

/**
 * Download the index and parse it
 * @param cache_file Where to keep the downloaded copy
 * @param allow_cached Fall back to the last downloaded copy if the download fails
 * @param stale Set to whether the cached copy was used, if not null
 **/
Result<plugin_index_t> fetch_plugin_index(const fs::path& cache_file, bool allow_cached, bool* stale = nullptr);

/**
 * Parse an index.toml. Entries this oshot can't make sense of are skipped
 * @param path The index file
 **/
Result<plugin_index_t> parse_plugin_index(const std::string& path);

/**
 * Check whether this oshot can load a version (ABI, ImGui and oshot version) and
 * install it on this platform (a prebuilt file for OSHOT_TARGET, or a source)
 * @param v The version
 * @param why_not Set to the reason it can't, if not null
 **/
bool is_installable(const index_version_t& v, std::string* why_not = nullptr);

/**
 * Pick the newest installable version that isn't yanked
 * @param plugin The plugin
 * @param why_not Set to why none can be installed, if not null
 * @return The version, or nullptr
 **/
const index_version_t* pick_version(const index_plugin_t& plugin, std::string* why_not = nullptr);

// ------------------------------
// Helpers
// ------------------------------

/**
 * Download a file with the curl program
 * @param url The URL
 * @param dest Where to write the file
 **/
Result<> download_file(const std::string& url, const fs::path& dest);

/**
 * Hash a file with SHA-256
 * @param path The file
 * @return The hash in lowercase hex
 **/
Result<std::string> sha256_file(const fs::path& path);

/**
 * Compare two semantic versions, like strcmp ("1.10.0" > "1.9.0" > "1.9.0-beta")
 * @param a The first version
 * @param b The second version
 * @return <0, 0 or >0
 **/
int compare_versions(const std::string_view a, const std::string_view b);

#endif
