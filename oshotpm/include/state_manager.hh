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
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "manifest.hh"
#include "toml_api.hpp"
#include "util.hpp"

// The text transformer chain as state.toml's [text-transformers] stores it, by plugin ID
struct text_transformers_t
{
    // The order they run in
    std::vector<std::string> order;

    // The ones switched off in Preferences, while their plugins stay enabled
    std::vector<std::string> disabled;

    bool operator==(const text_transformers_t&) const = default;
};

// state.toml: what's installed. One [repositories.<name>] table per source
// (git URL, local folder or archive) and one [plugins."<id>"] table per
// installed plugin, which points back at its repository. Also the order
// of the text transformers, in [text-transformers].
// Changes stay in memory until SaveState()
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

    // Adds or replaces a plugin, keeping its enabled state if it was installed
    void SetPlugin(const std::string& repo, const plugin_t& plugin, const std::string& library_file);

    // Adds or replaces a repository, without its plugins
    void SetRepo(const manifest_t& repo);

    // Removes the repository and the plugins it installed
    void RemoveRepo(const std::string_view name);

    [[nodiscard]] std::optional<manifest_t> FindRepo(std::string_view name) const;
    [[nodiscard]] std::optional<plugin_t>   FindPlugin(std::string_view id) const;

    // The [text-transformers] table, empty lists if it's missing
    [[nodiscard]] text_transformers_t GetTextTransformers() const;

    // Replaces the [text-transformers] table, leaving out the IDs that aren't installed
    void SetTextTransformers(const text_transformers_t& transformers);

    // Removes the plugin, and its ID from [text-transformers]
    void     RemovePlugin(const std::string_view id);
    Result<> SetEnabled(const std::string_view id, bool enabled);
    Result<> SaveState();

private:
    // GetAllRepos()'s result, rebuilt when m_is_changed
    mutable std::vector<manifest_t> m_manifests_cache;
    mutable bool                    m_is_changed{ true };

    const fs::path m_path{ get_config_dir() / "plugins" / "state.toml" };
    TomlAPI        m_toml;

    toml::table& Repos() { return TomlAPI::EnsureTable(m_toml.GetTbl(), "repositories"); }
    toml::table& Plugins() { return TomlAPI::EnsureTable(m_toml.GetTbl(), "plugins"); }
};

/**
 * Put the plugins that have a text transformer in the order the user saved.
 * The saved order comes first, then the plugins missing from it, sorted by ID.
 * IDs saved for plugins that aren't in `available` are skipped
 * @param available The IDs of the plugins with a transformer
 * @param saved What GetTextTransformers() returned
 * @return Each ID with whether its transformer is on
 **/
std::vector<std::pair<std::string, bool>> arrange_text_transformers(std::span<const std::string> available,
                                                                    const text_transformers_t&   saved);

#endif
