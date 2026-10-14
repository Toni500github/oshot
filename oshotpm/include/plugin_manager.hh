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

    // `default_answer` is both what a CLI prompt should suggest AND what a
    // non-interactive caller (--yes, or a GUI that can't block the frame on a
    // modal) should assume without asking. A shut-up implementation just
    // returns default_answer instead of the old `!options.install_shut_up &&
    // !ask_user_yn(...)` check being repeated at every call site.
    std::function<bool(const std::string_view prompt, bool default_answer)> confirm;
};

// ----- git operations -----
// Wraps the git subprocess calls PluginManager needs. Previously inlined
// TinyProcessLib::Process calls duplicated across AddPluginRepo/UpdateRepos.
class GitClient
{
public:
    GitClient(const PluginCallbacks& callbacks) : m_callbacks(callbacks) {}

    Result<> Clone(const std::string& url, const fs::path& dest_dir) const;
    Result<> PullRebase(const fs::path& repo_dir) const;
    Result<> RevParseUpstream(const fs::path& repo_dir, std::string& out_hash) const;
    Result<> LsRemoteHead(const std::string& url, std::string& out_hash) const;

private:
    const PluginCallbacks& m_callbacks;
};

// building a single plugin
enum class PluginBuildResult : std::uint8_t
{
    Built,
    SkippedUnsupportedPlatform,
};

// Platform check + ID-conflict check + running build_steps for one plugin.
// Knows nothing about repos-as-a-whole or where output ends up.
class PluginBuilder
{
public:
    PluginBuilder(const PluginCallbacks& callbacks) : m_callbacks(callbacks) {}

    // `is_conflicting` is injected instead of this class calling into
    // StateManager itself, so it stays decoupled from state-file concerns.
    Result<PluginBuildResult> Build(const plugin_t&                             plugin,
                                    bool                                        is_update,
                                    const std::function<bool(const plugin_t&)>& is_conflicting) const;

private:
    const PluginCallbacks& m_callbacks;
};

enum class ArchiveType : std::uint8_t
{
    None,
    Targz,
    Tarxz,
    Zip,
};

namespace Archiver
{
bool        IsArchive(const fs::path& archive);
ArchiveType GetArchiveType(const fs::path& archive);
Result<>    Extract(const fs::path& archive, const fs::path& dest_dir);

};  // namespace Archiver

// ----- installing a built plugin -----
// Moves a plugin's built output files into the config directory. Does NOT
// touch StateManager, it returns the installed library paths and lets
// PluginManager decide what to do with the state file.
class PluginInstaller
{
public:
    PluginInstaller(const PluginCallbacks& callbacks) : m_callbacks(callbacks) {}

    [[nodiscard]] Result<fs::path> InstallLibrary(const plugin_t& plugin,
                                                  const fs::path& library_dir,
                                                  const fs::path& manifest_config_path,
                                                  bool            force,
                                                  bool            is_update) const;

private:
    const PluginCallbacks& m_callbacks;
};

// ----- orchestrator -----

// Describes where a working directory passed to BuildPlugins() came from,
// which drives two independent things at once: whether oshotpm is allowed
// to delete/move it, and what RepoSource the resulting manifest_t gets
// tagged with.
enum class WorkingDirOrigin : std::uint8_t
{
    // A temp dir oshotpm created itself via `git clone`. Owned: cleaned up
    // on failure, renamed into the long-term cache on success. Tagged
    // RepoSource::GitRepository.
    GitClone,

    // A folder the caller (person, CLI arg) pointed us at directly. NOT
    // owned: never deleted or renamed, regardless of success or failure.
    // Tagged RepoSource::LocalPlugin.
    UserFolder,
};

class PluginManager
{
public:
    PluginManager(StateManager&& state_manager, PluginCallbacks& callbacks, bool is_cli)
        : m_is_cli(is_cli),
          m_state_manager(std::move(state_manager)),
          m_callbacks(callbacks),
          m_git(m_callbacks),
          m_builder(m_callbacks),
          m_installer(m_callbacks)
    {}

    Result<> ConfirmTrustDisclaimer() const;
    Result<> AddPluginRepo(const std::string& repo, bool is_update = false);
    Result<> UpdateRepos();
    Result<> RemoveRepo(const std::string& repo_name);
    Result<> BuildPlugins(const fs::path& working_dir, bool is_update, WorkingDirOrigin origin);
    Result<> InstallPrebuilt(const fs::path& archive, bool is_update = false);

    // Single entry point for the UI: figures out whether `source` is a git
    // URL, a local source folder, or a prebuilt release archive, and routes
    // to the matching install path.
    Result<> Install(const std::string& source, bool is_update = false);

    // Replaces the callback set used for all following operations. Safe to
    // call between operations; must not be called while a call into this
    // object is in flight on another thread, since GitClient/PluginBuilder/
    // PluginInstaller hold references into m_callbacks rather than copies.
    void SetCallbacks(PluginCallbacks callbacks) { m_callbacks = std::move(callbacks); }

    StateManager& GetStateManager() { return m_state_manager; }
    bool          IsPluginConflicting(const plugin_t& plugin) const;

private:
    Result<> ConfirmDependencies(const manifest_t& repo) const;
    Result<> BuildAllPlugins(const manifest_t& repo, bool is_update, std::vector<std::string>& out_skipped);
    Result<> FinalizeRepoDirectory(const fs::path& working_dir, const fs::path& repo_cache_path) const;
    Result<> InstallAllPlugins(const manifest_t&               repo,
                               const std::vector<std::string>& skipped,
                               bool                            is_update,
                               bool                            built_from_source);

    // Shared tail of BuildPlugins/InstallPrebuilt: copies built/prebuilt
    // libraries into the config dir and reports the final success message.
    // Everything before this point (parsing the manifest, trust/dependency
    // confirmation, and either building or platform-filtering the plugin
    // list) differs between the two callers.
    Result<> FinalizeInstall(const manifest_t&               repo,
                             const std::vector<std::string>& skipped,
                             bool                            is_update,
                             bool                            built_from_source);

    bool            m_is_cli;
    StateManager    m_state_manager;
    PluginCallbacks m_callbacks;
    GitClient       m_git;
    PluginBuilder   m_builder;
    PluginInstaller m_installer;
    fs::path        m_config_path{ get_config_dir() / "plugins" };
    fs::path        m_cache_path{ get_home_cache_dir() / "oshotpm" };
};

#endif
