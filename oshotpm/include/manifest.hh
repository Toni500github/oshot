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

#ifndef _MANIFEST_HH_
#define _MANIFEST_HH_

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "platform.hpp"
#include "toml_api.hpp"

#if OSHOT_LINUX
#  define OSHOT_PLATFORM "linux"
#elif defined(__FreeBSD__)
#  define OSHOT_PLATFORM "freebsd"
#elif defined(__OpenBSD__)
#  define OSHOT_PLATFORM "openbsd"
#elif defined(__NetBSD__)
#  define OSHOT_PLATFORM "netbsd"
#elif defined(__DragonFly__)
#  define OSHOT_PLATFORM "dragonfly"
#elif OSHOT_MACOS
#  define OSHOT_PLATFORM "macos"
#elif OSHOT_WINDOWS
#  define OSHOT_PLATFORM "windows"
#endif

// Where a repo entry came from. Git-sourced repos live under a cache dir
// oshotpm owns and can `git pull`/`git ls-remote` to check for updates.
// Local ones (a folder someone pointed us at, or a prebuilt archive) have
// no upstream to track: oshotpm never renames/deletes the source, and
// UpdateRepos() skips them outright instead of guessing from git_hash being
// empty.
enum class RepoSource : std::uint8_t
{
    GitRepository,
    LocalPlugin
};

struct plugin_t
{
    // The plugin name.
    // It must be conform to the function IsValidName()
    std::string name;

    // The plugin ID.
    // It must be conform to the function IsValidID()
    std::string id;

    // The plugin description.
    std::string description;

    // The plugin build directory,
    // where we'll retrieve the built plugin library.
    std::string output_dir;

    // The plugin SPDX Licenses Identifier (MIT, GPL-2.0, ...)
    // Not valided.
    std::vector<std::string> licenses;

    // The state 'repositories.repo-name.plugins.library' field.
    // NOTE: MUST be populated only by the state manager.
    fs::path library;

    // The plugin authors.
    std::vector<std::string> authors;

    // A list of commands to be executed for building the plugin.
    // Kinda like a Makefile target instructions.
    std::vector<std::string> build_steps;

    // Platforms that are supported by the plugin.
    // Make it a string and put 'all' for being cross-platform.
    std::vector<std::string> platforms;
};

struct manifest_t
{
    // The repository name.
    // It must be conform to the function is_valid_name()
    std::string name;

    // The repository git url
    std::string url;

    // NOTE: INTERNAL ONLY
    // The repository latest commit hash. Empty for a LocalPlugin source,
    // and may also be empty for a GitRepository source if the git lookup
    // wasn't possible (see Manifest::ParseManifest).
    std::string git_hash;

    // NOTE: INTERNAL ONLY. Stamped by PluginManager (not by Manifest
    // itself) once it knows how this manifest was obtained: Manifest has
    // no way to tell "cloned by us" apart from "just a folder that happens
    // to have a .git in it".
    RepoSource source = RepoSource::GitRepository;

    // An array of all the plugins that are declared in the manifest
    std::vector<plugin_t> plugins;

    // An array for storing the dependencies for 'all' and current platforms.
    // first -> platform string name
    // seconds -> platform dependencies vector names
    std::vector<std::string> dependencies;
};

constexpr char const MANIFEST_NAME[] = "oshot-plugin.toml";  // NOLINT

class Manifest
{
public:
    Manifest(fs::path path);

    [[nodiscard]] plugin_t GetPlugin(const std::string_view name) const;
    Result<>               ParseManifest();

    [[nodiscard]] const manifest_t& GetRepo() const { return m_repo; }
    [[nodiscard]] bool              IsParsed() const { return m_is_parsed; }
    static bool                     IsValidID(const std::string_view id);
    static bool                     IsValidName(const std::string_view name);

private:
    fs::path   m_path;
    TomlAPI    m_toml;
    manifest_t m_repo;

    bool m_is_parsed{};
};

#endif  // !_MANIFEST_HH_;
