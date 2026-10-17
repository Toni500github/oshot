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
#include <map>
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

#if defined(__x86_64__) || defined(_M_X64)
#  define OSHOT_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define OSHOT_ARCH "aarch64"
#elif defined(__i386__) || defined(_M_IX86)
#  define OSHOT_ARCH "i686"
#elif defined(__arm__) || defined(_M_ARM)
#  define OSHOT_ARCH "armv7"
#elif defined(__riscv) && __riscv_xlen == 64
#  define OSHOT_ARCH "riscv64"
#elif defined(__powerpc64__)
#  define OSHOT_ARCH "ppc64le"
#elif defined(__s390x__)
#  define OSHOT_ARCH "s390x"
#else
#  define OSHOT_ARCH "unknown"
#endif

// Key of a prebuilt library in a manifest, e.g. "linux-x86_64"
#define OSHOT_TARGET OSHOT_PLATFORM "-" OSHOT_ARCH

// Where a repo entry came from. Git-sourced repos live under a cache dir
// oshotpm owns and can fetch to check for updates. Local ones (a folder
// someone pointed us at, or a prebuilt archive) have no upstream to track.
enum class RepoSource : std::uint8_t
{
    GitRepository,
    LocalPlugin,
    Index,  // the plugin index: its plugins are tracked one by one, by version
};

struct plugin_t
{
    // The plugin name, the manifest's table name. See Manifest::IsValidName()
    std::string name;

    // The plugin ID, the same as the one compiled into the library. See Manifest::IsValidID()
    std::string id;

    std::string description;

    // Source builds: the directory, relative to the repository, where the
    // build steps leave the library. Exactly one library must be in it.
    std::string output_dir;

    // Prebuilt archives: library path per target ("linux-x86_64" -> "linux/libfoo.so")
    std::map<std::string, std::string> prebuilt;

    // SPDX license identifiers (MIT, GPL-2.0, ...). Not validated.
    std::vector<std::string> licenses;

    std::vector<std::string> authors;

    // Commands that build the plugin, already picked for this platform
    // ("build-steps-<platform>" over "build-steps")
    std::vector<std::string> build_steps;

    // Platforms the plugin supports, or "all"
    std::vector<std::string> platforms;

    // Only set for installed plugins, by StateManager
    std::string repository;
    std::string version;  // installed from the plugin index only
    fs::path    library;
    bool        enabled = true;
};

struct manifest_t
{
    // The repository name. See Manifest::IsValidName()
    std::string name;

    // The repository git url or homepage
    std::string url;

    // The installed commit. Only for RepoSource::GitRepository
    std::string git_hash;

    RepoSource source = RepoSource::GitRepository;

    std::vector<plugin_t> plugins;

    // Packages needed to build the plugins here, for "all" and this platform.
    // Only shown to the person, never installed.
    std::vector<std::string> dependencies;
};

constexpr char const MANIFEST_NAME[] = "oshot-plugin.toml";  // NOLINT

class Manifest
{
public:
    Manifest(fs::path path);

    Result<> ParseManifest();

    [[nodiscard]] const manifest_t& GetRepo() const { return m_repo; }
    [[nodiscard]] bool              IsParsed() const { return m_is_parsed; }

    // Letters, digits, '-', '_' and '=', 1-64 characters
    static bool IsValidName(const std::string_view name);

    // Lowercase letters, digits, '-', '_' and '.', at least one '.', 1-64
    // characters, starting and ending with a letter or digit, no ".."
    static bool IsValidID(const std::string_view id);
    static bool IsSupported(const plugin_t& plugin);

private:
    [[nodiscard]] Result<plugin_t> GetPlugin(const std::string_view name) const;

    fs::path   m_path;
    TomlAPI    m_toml;
    manifest_t m_repo;

    bool m_is_parsed{};
};

#endif  // !_MANIFEST_HH_;
