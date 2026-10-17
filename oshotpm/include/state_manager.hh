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

#ifndef _STATE_MANAGER_HH_
#define _STATE_MANAGER_HH_

#include <optional>

#include "manifest.hh"
#include "toml_api.hpp"
#include "util.hpp"

// state.toml: what's installed. One [repositories.<name>] table per source
// (git URL, local folder or archive) and one [plugins."<id>"] table per
// installed plugin, which points back at its repository.
class StateManager
{
public:
    StateManager();
    StateManager(StateManager&&)                 = default;
    StateManager(const StateManager&)            = delete;
    StateManager& operator=(const StateManager&) = delete;
    ~StateManager()                              = default;

    // Re-reads state.toml, e.g. after oshotpm changed it while oshot runs
    void Reload();

    // Grouped by repository, plugins sorted by ID
    [[nodiscard]] const std::vector<manifest_t>& GetAllRepos() const;
    [[nodiscard]] std::optional<manifest_t>      FindRepo(std::string_view name) const;
    [[nodiscard]] std::optional<plugin_t>        FindPlugin(std::string_view id) const;

    void SetRepo(const manifest_t& repo);
    // Adds or replaces a plugin, keeping its enabled state if it was installed
    void SetPlugin(const std::string& repo, const plugin_t& plugin, const std::string& library_file);
    void RemovePlugin(std::string_view id);
    // Removes the repository and the plugins it installed
    void     RemoveRepo(std::string_view name);
    Result<> SetEnabled(std::string_view id, bool enabled);

    Result<> SaveState();

private:
    const fs::path m_path{ get_config_dir() / "plugins" / "state.toml" };
    TomlAPI        m_toml;

    mutable std::vector<manifest_t> m_manifests_cache;
    mutable bool                    m_is_changed{ true };

    toml::table& Repos() { return TomlAPI::EnsureTable(m_toml.GetTbl(), "repositories"); }
    toml::table& Plugins() { return TomlAPI::EnsureTable(m_toml.GetTbl(), "plugins"); }
};

#endif
