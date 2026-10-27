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

#include "manifest.hh"

#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

#include "fmt/ranges.h"
#include "util.hpp"

static constexpr auto PLATFORMS = std::to_array<std::string_view>(
    { "all", "linux", "macos", "windows", "freebsd", "openbsd", "netbsd", "dragonfly" });

bool Manifest::IsValidName(const std::string_view name)
{
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](const unsigned char c) {
        return (std::isalnum(c) || c == '-' || c == '_' || c == '=');
    });
}

// Based on geode-sdk's ModMetadata::validateID(), but allows more than one dot
bool Manifest::IsValidID(const std::string_view id)
{
    if (id.empty() || id.size() > 64)
        return false;

    const auto is_alnum = [](const char c) { return ('a' <= c && c <= 'z') || ('0' <= c && c <= '9'); };
    if (!is_alnum(id.front()) || !is_alnum(id.back()) || id.find("..") != id.npos)
        return false;

    bool found_dot = false;
    for (const char c : id)
    {
        if (!(is_alnum(c) || c == '-' || c == '_' || c == '.'))
            return false;
        if (c == '.')
            found_dot = true;
    }

    return found_dot;
}

bool Manifest::IsSupported(const plugin_t& plugin)
{
    return plugin.platforms.empty() || std::ranges::find(plugin.platforms, "all") != plugin.platforms.end() ||
           std::ranges::find(plugin.platforms, OSHOT_PLATFORM) != plugin.platforms.end();
}

std::string capability_label(const std::string_view capability)
{
    if (capability == "network")
        return "uses the internet";
    if (capability == "files")
        return "uses your files";
    if (capability == "processes")
        return "runs programs";
    return std::string(capability);
}

std::string capabilities_notice(const std::string_view name, const std::map<std::string, std::string>& capabilities)
{
    if (capabilities.empty())
        return {};

    std::string notice = fmt::format("According to its author, '{}':", name);
    for (const auto& [capability, reason] : capabilities)
    {
        const std::string_view what = capability == "network"     ? "connects to the internet"
                                      : capability == "files"     ? "reads or writes your files"
                                      : capability == "processes" ? "runs other programs"
                                                                  : std::string_view(capability);
        notice += fmt::format("\n- {}: {}", what, reason);
    }
    return notice;
}

Manifest::Manifest(fs::path path) : m_path(std::move(path))
{}

Result<> Manifest::ParseManifest()
{
    if (IsParsed())
        return Ok();

    std::error_code ec;
    if (!fs::exists(m_path, ec))
        return Err("'{}' not found", m_path.string());

    TRY(m_toml.LoadFile(m_path.string()));

    m_repo.name = m_toml.GetValue<std::string>("repository.name", "", true);
    m_repo.url  = m_toml.GetValue<std::string>("repository.url", "", true);
    if (m_repo.name.empty())
        return Err("The manifest has no [repository] name");
    if (!IsValidName(m_repo.name))
        return Err("Repository name '{}' is invalid: use 1-64 letters, digits, '-', '_' or '='", m_repo.name);
    if (m_repo.name == "index")
        return Err("The repository name 'index' is reserved for plugins from the plugin index");

    const auto& deps_all  = m_toml.GetValueArrayStr("dependencies.all", {});
    const auto& deps_plat = m_toml.GetValueArrayStr("dependencies." OSHOT_PLATFORM, {});
    m_repo.dependencies.insert(m_repo.dependencies.end(), deps_all.begin(), deps_all.end());
    m_repo.dependencies.insert(m_repo.dependencies.end(), deps_plat.begin(), deps_plat.end());

    std::vector<std::string> errors;
    for (const auto& [key, node] : m_toml.GetTbl())
    {
        const std::string_view name = key.str();
        if (name == "repository" || name == "dependencies")
            continue;
        if (!node.is_table())
        {
            errors.push_back(fmt::format("'{}' is not a table", name));
            continue;
        }

        Result<plugin_t> plugin = GetPlugin(name);
        if (!plugin.ok())
        {
            errors.push_back(plugin.error_v());
            continue;
        }

        if (std::ranges::find(m_repo.plugins, plugin.get().id, &plugin_t::id) != m_repo.plugins.end())
        {
            errors.push_back(fmt::format("plugin ID '{}' is used twice", plugin.get().id));
            continue;
        }

        m_repo.plugins.push_back(std::move(plugin.get()));
    }

    if (!errors.empty())
        return Err("Invalid {}:\n  {}", MANIFEST_NAME, fmt::join(errors, "\n  "));
    if (m_repo.plugins.empty())
        return Err("{} doesn't declare any plugin", MANIFEST_NAME);

    m_is_parsed = true;
    return Ok();
}

Result<plugin_t> Manifest::GetPlugin(const std::string_view name) const
{
    if (!IsValidName(name))
        return Err("plugin name '{}' is invalid: use 1-64 letters, digits, '-', '_' or '='", name);

    const toml::table& tbl = *m_toml.GetTbl()[name].as_table();
    const TomlAPI      api(tbl);

    plugin_t plugin;
    plugin.name        = std::string(name);
    plugin.id          = tbl["id"].value_or(std::string());
    plugin.description = tbl["description"].value_or(std::string());
    plugin.output_dir  = tbl["output-dir"].value_or(std::string());
    plugin.licenses    = api.GetValueArrayStr("licenses", {});
    plugin.authors     = api.GetValueArrayStr("authors", {});
    plugin.platforms   = api.GetValueArrayStr("platforms", {});
    plugin.build_steps = api.GetValueArrayStr("build-steps-" OSHOT_PLATFORM, api.GetValueArrayStr("build-steps", {}));

    if (const toml::table* prebuilt = tbl["prebuilt"].as_table())
        for (const auto& [target, path] : *prebuilt)
            if (const auto* str = path.as_string())
                plugin.prebuilt.emplace(target.str(), str->get());

    if (const toml::node* caps = tbl.get("capabilities"))
    {
        if (!caps->is_table())
            return Err("plugin '{}': capabilities must be a table, like [{}.capabilities]", name, name);
        for (const auto& [capability, reason] : *caps->as_table())
        {
            const auto* str = reason.as_string();
            if (!str || str->get().empty())
                return Err("plugin '{}': the capability '{}' needs a reason, like {} = \"Downloads translations\"",
                           name,
                           capability.str(),
                           capability.str());
            plugin.capabilities.emplace(capability.str(), str->get());
        }
    }

    if (!IsValidID(plugin.id))
        return Err(
            "plugin '{}' has an invalid ID '{}': use lowercase letters, digits, '-', '_' and at least one "
            "'.', like \"io.github.user.plugin\"",
            name,
            plugin.id);

    for (const std::string& platform : plugin.platforms)
        if (std::ranges::find(PLATFORMS, platform) == PLATFORMS.end())
            return Err(
                "plugin '{}' lists the unknown platform '{}' (known: {})", name, platform, fmt::join(PLATFORMS, ", "));

    return Ok(std::move(plugin));
}
