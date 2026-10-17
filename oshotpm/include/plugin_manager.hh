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

#ifndef _PLUGIN_MANAGER_HH_
#define _PLUGIN_MANAGER_HH_

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "manifest.hh"
#include "state_manager.hh"
#include "util.hpp"

// options
inline struct operations_t
{
    bool install_force   = false;
    bool install_shut_up = false;
    bool list_verbose    = false;

    std::vector<std::string> arguments;
} options;

// ----- notifications / confirmation -----
struct PluginCallbacks
{
    std::function<void(const std::string_view)> on_status;
    std::function<void(const std::string_view)> on_success;
    std::function<void(const std::string_view)> on_warning;
    std::function<void(const std::string_view)> on_error;
    std::function<void(const std::string_view)> on_info;
    // Raw output of git and of build steps, as it arrives. Optional.
    std::function<void(const std::string_view)> on_output;

    // `default_answer` is both what a CLI prompt should suggest AND what a
    // non-interactive caller (--yes) should assume without asking.
    std::function<bool(const std::string_view prompt, bool default_answer)> confirm;
};

class PluginManager
{
public:
    PluginManager(StateManager&& state_manager, PluginCallbacks& callbacks, bool is_cli)
        : m_is_cli(is_cli), m_state_manager(std::move(state_manager)), m_callbacks(callbacks)
    {}

    // `source` is a git URL, a local folder with an oshot-plugin.toml, or a
    // prebuilt archive (.zip, .tar.gz/.tgz, .tar.xz/.txz)
    Result<> Install(const std::string& source);

    // Fetches every repository installed from git, and rebuilds the ones that changed
    Result<> UpdateAll();

    // `name` is a plugin ID (has a dot) or a repository name (has none)
    Result<> Uninstall(const std::string& name);

    // `name` is a plugin ID or "<repository>/<plugin name>"
    Result<> SetEnabled(const std::string& name, bool enabled);

    // Replaces the callback set used for all following operations. Must not be
    // called while an operation runs on another thread.
    void SetCallbacks(PluginCallbacks callbacks) { m_callbacks = std::move(callbacks); }

    StateManager& GetStateManager() { return m_state_manager; }

    // The plugin SDK (include/oshot/*.h, and lib/ on Windows) that came with
    // this oshot, or empty if there's none. $OSHOT_SDK overrides it.
    static fs::path FindSdk();

private:
    enum class Origin : std::uint8_t
    {
        GitClone,    // a clone in oshotpm's cache
        UserFolder,  // the person's own folder, built in place and never deleted
        Archive,     // an extracted prebuilt archive: nothing is built
    };

    struct repo_source_t
    {
        Origin      origin;
        std::string git_url;   // GitClone only
        std::string git_hash;  // GitClone only
    };

    Result<> InstallGit(const std::string& url);
    Result<> InstallArchive(const fs::path& archive);
    Result<> UpdateRepo(const manifest_t& repo);

    // Builds (or, for archives, finds) and installs every plugin of the
    // manifest in `dir`. Sets `out_name` to the repository name once known.
    Result<> InstallFromDir(const fs::path& dir, const repo_source_t& src, bool is_update, std::string& out_name);

    Result<fs::path>    BuildPlugin(const plugin_t& plugin, const fs::path& dir, const manifest_t& repo);
    Result<fs::path>    FindPrebuilt(const plugin_t& plugin, const fs::path& dir);
    Result<std::string> CopyLibrary(const std::string& id, const fs::path& library);

    Result<> ConfirmTrust();
    Result<> ConfirmDependencies(const manifest_t& repo);

    Result<std::string> Git(const std::vector<std::string>& args, const fs::path& cwd);
    Result<>            CheckGit();

    void     RemoveStaleWorkDirs();
    fs::path NewWorkDir(std::string_view prefix);
    void     RemovePluginDir(const std::string& id);

    bool            m_is_cli;
    StateManager    m_state_manager;
    PluginCallbacks m_callbacks;

    fs::path m_plugins_path{ get_config_dir() / "plugins" };
    fs::path m_cache_path{ get_home_cache_dir() / "oshotpm" };
};

#endif
