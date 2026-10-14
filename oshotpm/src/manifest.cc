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
#include <cctype>
#include <filesystem>
#include <vector>

#include "tiny-process-library/process.hpp"
#include "util.hpp"

bool Manifest::IsValidName(const std::string_view name)
{
    return (!name.empty() || std::ranges::all_of(name, [](const unsigned char c) {
        return (isalnum(c) || c == '-' || c == '_' || c == '=');
    }));
}

// Taken from geode-sdk's ModMetadata::validateID()
// But we allow multiple dots instead of one
bool Manifest::IsValidID(const std::string_view id)
{
    // IDs may not be empty nor exceed 64 characters
    if (id.size() == 0 || id.size() > 64)
        return false;

    bool found_dot = false;
    for (const char c : id)
    {
        if (!(('a' <= c && c <= 'z') || ('0' <= c && c <= '9') || (c == '-' || c == '_' || c == '.')))
            return false;
        if (c == '.')
            found_dot = true;
    }

    // At least one dot required
    return found_dot;
}

Manifest::Manifest(fs::path path) : m_path(std::move(path))
{}

Result<> Manifest::ParseManifest()
{
    if (IsParsed())
        return Ok();

    if (!fs::exists(m_path))
        return Err("Path '{}' doesn't exist", m_path.string());

    TRY(m_toml.LoadFile(m_path.string()));
    static std::string str_stderr;

    m_repo.name = m_toml.GetValue<std::string>("repository.name", UNKNOWN);
    m_repo.url  = m_toml.GetValue<std::string>("repository.url", "");
    if (m_repo.name.empty() || m_repo.name == UNKNOWN)
        return Err("Couldn't find manifest repository name");
    if (!IsValidName(m_repo.name))
        return Err(
            "Manifest repository name '{}' is invalid. Only alphanumeric and '-', '_', '=' are allowed in the name",
            m_repo.name);

    // Only a git checkout (a repo cloned by AddPluginRepo, or a person's own
    // git-tracked source folder) has a hash to read. A prebuilt release
    // archive extraction, or a local folder that's just source code without
    // git, legitimately has none: leave git_hash empty rather than failing
    // the whole parse over it. Downstream, an empty git_hash is what tells
    // UpdateRepos() there's nothing to `git pull`/`git ls-remote` here.
    if (fs::exists(m_path.parent_path() / ".git"))
    {
        TinyProcessLib::Process proc(
            fmt::format("git -C {} rev-parse HEAD", m_path.parent_path().string()),
            "",
            [&](const char* buf, size_t len) { m_repo.git_hash.assign(buf, len); },
            [&](const char* buf, size_t len) { str_stderr.append(buf, len); });
        if (proc.get_exit_status() != 0)
            return Err("Failed to get manifest git repository hash: {}", str_stderr);
        std::erase(m_repo.git_hash, '\n');
    }

    const auto& deps_all  = m_toml.GetValueArrayStr("dependencies.all", {});
    const auto& deps_plat = m_toml.GetValueArrayStr("dependencies." OSHOT_PLATFORM, {});
    m_repo.dependencies.insert(m_repo.dependencies.end(), deps_all.begin(), deps_all.end());
    m_repo.dependencies.insert(m_repo.dependencies.end(), deps_plat.begin(), deps_plat.end());

    Result<> invalidated_err = Ok();
    for (const auto& [name, _] : m_toml.GetTbl())
    {
        if (name.str() == "repository" || name.str() == "dependencies")
            continue;

        if (!IsValidName(name.str()))
        {
            invalidated_err =
                Err("Plugin '{}' has an invalid name. Only alphanumeric and '-', '_', '=' are allowed in the name",
                    name.str());
            continue;
        }

        plugin_t plugin = GetPlugin(name);
        if (!IsValidID(plugin.id))
        {
            invalidated_err =
                Err("Plugin '{}' has an invalid ID. Only alphanumeric and '-', '_', '=', '.' are allowed in the ID",
                    plugin.id);
            continue;
        }

        m_repo.plugins.emplace_back(std::move(plugin));
    }

    return invalidated_err;
}

plugin_t Manifest::GetPlugin(const std::string_view name) const
{
    return { .name        = std::string(name),
             .id          = m_toml.GetValueFromTable<std::string>(name, "id", UNKNOWN),
             .description = m_toml.GetValueFromTable<std::string>(name, "description", UNKNOWN),
             .output_dir  = m_toml.GetValueFromTable<std::string>(name, "output-dir", UNKNOWN),
             .licenses    = m_toml.GetValueArrayStr(name, "licenses", {}),
             .library     = {},  // MUST be populated only in StateManager
             .authors     = m_toml.GetValueArrayStr(name, "authors", {}),
             .build_steps = m_toml.GetValueArrayStr(name, "build-steps", {}),
             .platforms   = m_toml.GetValueArrayStr(name, "platforms", {}) };
}
