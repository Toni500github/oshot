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

#include "state_manager.hh"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string_view>

#include "manifest.hh"

static constexpr std::string_view STATE_HEADER =
    "# Installed oshot plugins, written by oshot and oshotpm.\n"
    "# Change it with `oshotpm` or the Plugins menu in oshot instead of by hand.\n";

StateManager::StateManager()
{
    Reload();
}

void StateManager::Reload()
{
    m_is_changed = true;
    m_toml       = TomlAPI();

    std::error_code ec;
    if (!fs::exists(m_path, ec))
        return;

    // A broken state file shouldn't keep oshot from starting: plugins just don't load
    MUST_OK(m_toml.LoadFile(m_path.string()), spdlog::error("Failed to read {}: {}", m_path.string(), _r.error_v()));
}

static std::vector<std::string> str_array(const toml::table& tbl, std::string_view key)
{
    return TomlAPI(tbl).GetValueArrayStr(key, {});
}

const std::vector<manifest_t>& StateManager::GetAllRepos() const
{
    // Cache the state and update only if anything has changed
    if (!m_is_changed)
        return m_manifests_cache;
    m_manifests_cache.clear();
    m_is_changed = false;

    const toml::table* repositories = m_toml.GetTbl()["repositories"].as_table();
    const toml::table* plugins      = m_toml.GetTbl()["plugins"].as_table();
    if (!repositories)
        return m_manifests_cache;

    for (const auto& [repo_name, repo_node] : *repositories)
    {
        const toml::table* repo_tbl = repo_node.as_table();
        if (!repo_tbl)
            continue;

        manifest_t manifest;
        manifest.name     = repo_name.str();
        manifest.url      = (*repo_tbl)["url"].value_or(std::string());
        manifest.git_hash = (*repo_tbl)["git-hash"].value_or(std::string());
        manifest.source   = (*repo_tbl)["source"].value_or(std::string("git")) == "git" ? RepoSource::GitRepository
                                                                                        : RepoSource::LocalPlugin;
        m_manifests_cache.push_back(std::move(manifest));
    }

    if (plugins)
    {
        for (const auto& [id, plugin_node] : *plugins)
        {
            const toml::table* tbl = plugin_node.as_table();
            if (!tbl)
                continue;

            const std::string repo = (*tbl)["repository"].value_or(std::string());
            auto              it   = std::ranges::find(m_manifests_cache, repo, &manifest_t::name);
            if (it == m_manifests_cache.end())
                continue;

            plugin_t plugin;
            plugin.id          = id.str();
            plugin.name        = (*tbl)["name"].value_or(std::string());
            plugin.description = (*tbl)["description"].value_or(std::string());
            plugin.enabled     = (*tbl)["enabled"].value_or(true);
            plugin.authors     = str_array(*tbl, "authors");
            plugin.licenses    = str_array(*tbl, "licenses");
            plugin.platforms   = str_array(*tbl, "platforms");
            plugin.repository  = repo;

            const std::string library = (*tbl)["library"].value_or(std::string());
            if (!library.empty())
                plugin.library = get_config_dir() / "plugins" / plugin.id / fs::path(library).filename();

            it->plugins.push_back(std::move(plugin));
        }
    }

    for (manifest_t& m : m_manifests_cache)
        std::ranges::sort(m.plugins, {}, &plugin_t::id);

    return m_manifests_cache;
}

std::optional<manifest_t> StateManager::FindRepo(std::string_view name) const
{
    for (const manifest_t& m : GetAllRepos())
        if (m.name == name)
            return m;
    return std::nullopt;
}

std::optional<plugin_t> StateManager::FindPlugin(std::string_view id) const
{
    for (const manifest_t& m : GetAllRepos())
        for (const plugin_t& p : m.plugins)
            if (p.id == id)
                return p;
    return std::nullopt;
}

void StateManager::SetRepo(const manifest_t& repo)
{
    toml::table& tbl = TomlAPI::EnsureTable(Repos(), repo.name);
    tbl.insert_or_assign("url", repo.url);
    tbl.insert_or_assign("git-hash", repo.git_hash);
    tbl.insert_or_assign("source", repo.source == RepoSource::LocalPlugin ? "local" : "git");
    m_is_changed = true;
}

void StateManager::SetPlugin(const std::string& repo, const plugin_t& plugin, const std::string& library_file)
{
    toml::table& plugins = Plugins();

    bool enabled = true;
    if (const toml::table* old = plugins[plugin.id].as_table())
        enabled = (*old)["enabled"].value_or(true);

    plugins.insert_or_assign(plugin.id,
                             toml::table{ { "repository", repo },
                                          { "name", plugin.name },
                                          { "description", plugin.description },
                                          { "authors", TomlAPI::VectorToArray(plugin.authors) },
                                          { "licenses", TomlAPI::VectorToArray(plugin.licenses) },
                                          { "platforms", TomlAPI::VectorToArray(plugin.platforms) },
                                          { "library", library_file },
                                          { "enabled", enabled } });
    m_is_changed = true;
}

void StateManager::RemovePlugin(std::string_view id)
{
    Plugins().erase(id);
    m_is_changed = true;
}

void StateManager::RemoveRepo(std::string_view name)
{
    toml::table&             plugins = Plugins();
    std::vector<std::string> ids;
    for (const auto& [id, node] : plugins)
        if (const toml::table* tbl = node.as_table(); tbl && (*tbl)["repository"].value_or(std::string()) == name)
            ids.emplace_back(id.str());
    for (const std::string& id : ids)
        plugins.erase(id);

    Repos().erase(name);
    m_is_changed = true;
}

Result<> StateManager::SetEnabled(std::string_view id, bool enabled)
{
    toml::table* tbl = Plugins()[id].as_table();
    if (!tbl)
        return Err("No plugin with ID '{}' is installed", id);

    tbl->insert_or_assign("enabled", enabled);
    m_is_changed = true;
    return Ok();
}

Result<> StateManager::SaveState()
{
    std::error_code ec;
    fs::create_directories(m_path.parent_path(), ec);

    std::stringstream ss;
    ss << STATE_HEADER << m_toml.GetTbl() << '\n';
    if (!m_toml.SaveFile(ss.str(), m_path.string()))
        return Err("Failed to write '{}'", m_path.string());
    return Ok();
}
