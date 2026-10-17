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
#include "plugin_index.hh"
#include "state_manager.hh"
#include "util.hpp"

// oshotpm's command line options
struct operations_t
{
    // -f: reinstall plugins that are already installed
    bool install_force = false;

    // -y: don't ask for confirmations
    bool install_shut_up = false;

    // -v: more details in `list`
    bool list_verbose = false;

    std::vector<std::string> arguments;
};

// How PluginManager reports progress and asks questions: on the terminal in
// oshotpm, in the install status window in the GUI
struct PluginCallbacks
{
    // Raw output of git and of build steps, as it arrives. Optional.
    std::function<void(const std::string_view)> on_output;

    // `default_answer` is both what a CLI prompt should suggest AND what a
    // non-interactive caller (--yes) should assume without asking.
    std::function<bool(const std::string_view prompt, bool default_answer)> confirm;

    std::function<void(const std::string_view)> on_status;
    std::function<void(const std::string_view)> on_success;
    std::function<void(const std::string_view)> on_warning;
    std::function<void(const std::string_view)> on_error;
    std::function<void(const std::string_view)> on_info;
};

inline operations_t options;

// Installs, updates, uninstalls and enables plugins, keeping state.toml up to date.
// Used by oshotpm and by the GUI's plugin windows
class PluginManager
{
public:
    /**
     * @param state_manager The installed plugins
     * @param callbacks Where progress and questions go
     * @param is_cli oshotpm: warnings are printed before a short prompt, instead of
     *               all going in the confirmation like in the GUI
     **/
    PluginManager(StateManager&& state_manager, PluginCallbacks& callbacks, bool is_cli)
        : m_is_cli(is_cli), m_state_manager(std::move(state_manager)), m_callbacks(callbacks)
    {}

    // `source` is a plugin ID from the plugin index, a git URL, a local folder
    // with an oshot-plugin.toml, or a prebuilt archive (.zip, .tar.gz/.tgz, .tar.xz/.txz)
    Result<> Install(const std::string& source);

    // Installs the newest version of a plugin in the plugin index that works here
    Result<> InstallFromIndex(const std::string& id, bool is_update = false);

    // Updates the plugins from the index, and fetches every repository
    // installed from git and rebuilds the ones that changed
    Result<> UpdateAll();

    // Downloads the plugin index. With `allow_cached`, falls back to the last
    // downloaded copy when offline, and sets `stale`.
    static Result<plugin_index_t> FetchIndex(bool allow_cached, bool* stale = nullptr);

    // `name` is a plugin ID (has a dot) or a repository name (has none)
    Result<> Uninstall(const std::string& name);

    // `name` is a plugin ID or "<repository>/<plugin name>"
    Result<> SetEnabled(const std::string& name, bool enabled);

    // Replaces the callback set used for all following operations. Must not be
    // called while an operation runs on another thread.
    void SetCallbacks(PluginCallbacks callbacks) { m_callbacks = std::move(callbacks); }

    // The plugin SDK (include/oshot/*.h, and lib/ on Windows) that came with
    // this oshot, or empty if there's none. $OSHOT_SDK overrides it.
    static fs::path FindSdk();

    StateManager& GetStateManager() { return m_state_manager; }

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

    bool            m_is_cli;
    StateManager    m_state_manager;
    PluginCallbacks m_callbacks;

    fs::path m_plugins_path{ get_config_dir() / "plugins" };
    fs::path m_cache_path{ get_home_cache_dir() / "oshotpm" };

    // Builds (or, for archives, finds) and installs every plugin of the
    // manifest in `dir`. Sets `out_name` to the repository name once known.
    Result<> InstallFromDir(const fs::path& dir, const repo_source_t& src, bool is_update, std::string& out_name);

    // Copies the library into the plugin's directory and returns its file name
    Result<std::string> CopyLibrary(const std::string& id, const fs::path& library);

    // Asks whether to trust the plugin. `notice`: more to warn about, e.g. that the plugin uses the network
    Result<> ConfirmTrust(std::string_view notice = {});

    // Lists the packages the manifest needs and asks whether they're installed
    Result<> ConfirmDependencies(const manifest_t& repo);

    // Runs git in `cwd` and returns its trimmed output
    Result<std::string> Git(const std::vector<std::string>& args, const fs::path& cwd);

    // Deletes the work dirs older than an hour, left by an oshotpm that got killed or interrupted
    void RemoveStaleWorkDirs();

    Result<> InstallGit(const std::string& url);
    Result<> InstallIndexVersion(const index_plugin_t& plugin, const index_version_t& version, bool is_update);
    Result<> InstallArchive(const fs::path& archive);
    Result<> UpdateIndexPlugins(const manifest_t& repo);
    Result<> UpdateRepo(const manifest_t& repo);

    Result<fs::path> BuildPlugin(const plugin_t& plugin, const fs::path& dir, const manifest_t& repo);
    Result<fs::path> FindPrebuilt(const plugin_t& plugin, const fs::path& dir);

    Result<> CheckGit();

    fs::path NewWorkDir(std::string_view prefix);
    void     RemovePluginDir(const std::string& id);
};

#endif
