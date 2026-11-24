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

#include "plugin_index.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>

#include "fmt/format.h"
#include "libs/sha256.h"
#include "manifest.hh"
#include "plugin.hpp"
#include "tiny-process-library/process.hpp"
#include "toml_api.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
#include "util/strings.hpp"

// NOLINTBEGIN(modernize-avoid-c-arrays)
static constexpr char const DEFAULT_INDEX_URL[] = "https://toni500github.github.io/oshot-plugin-index/index.toml";
static constexpr char const MAX_DOWNLOAD[]      = "209715200";  // 200 MiB
static constexpr int64_t    INDEX_SCHEMA        = 1;
// NOLINTEND(modernize-avoid-c-arrays)

const index_version_t* index_plugin_t::FindVersion(const std::string_view version) const
{
    auto it = std::ranges::find(versions, version, &index_version_t::version);
    return it != versions.end() ? &*it : nullptr;
}

const index_plugin_t* plugin_index_t::Find(const std::string_view id) const
{
    auto it = std::ranges::find(plugins, id, &index_plugin_t::id);
    return it != plugins.end() ? &*it : nullptr;
}

std::string plugin_index_url()
{
    if (const char* env = std::getenv("OSHOT_PLUGIN_INDEX"); env && *env)
        return env;
    return DEFAULT_INDEX_URL;
}

Result<> download_file(const std::string& url, const fs::path& dest)
{
    std::string output;
    std::mutex  mtx;
    auto        collect = [&](const char* buf, size_t n) {
        std::scoped_lock lock(mtx);
        output.append(buf, n);
    };

    // curl ships with Windows 10+, macOS and nearly every Linux distribution
    TinyProcessLib::Process proc(
        { "curl", "-fsSL", "--retry", "2", "--max-filesize", MAX_DOWNLOAD, "-o", dest.string(), url },
        "",
        collect,
        collect);

    const int status = proc.get_exit_status();
    if (status != 0)
    {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
            output.pop_back();
        if (output.empty())
            output = "is curl installed?";
        return Err("Failed to download {} (curl exit code {}): {}", url, status, output);
    }
    return Ok();
}

namespace
{
class Sha256
{
public:
    Sha256() { sha256_init(&m_ctx); }

    void Update(const uint8_t* data, size_t len) { sha256_update(&m_ctx, data, len); }

    std::string HexDigest()
    {
        std::array<uint8_t, SHA256_BLOCK_SIZE> hash{};
        sha256_final(&m_ctx, hash.data());

        std::string hex;
        hex.reserve(hash.size() * 2);
        for (const uint8_t b : hash)
            hex += fmt::format("{:02x}", b);
        return hex;
    }

private:
    SHA256_CTX m_ctx;
};
}  // namespace

Result<std::string> sha256_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Err("Can't read '{}'", path.string());

    Sha256                       sha;
    std::array<char, UINT16_MAX> buf{};
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0)
        sha.Update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(in.gcount()));
    return Ok(sha.HexDigest());
}

// ----- versions -----
int compare_versions(std::string_view a, std::string_view b)
{
    // "1.2.3-rc.1" -> {1, 2, 3} and "rc.1"
    const auto split = [](std::string_view v) {
        Parser              p(v);
        std::array<long, 3> nums{};
        for (long& n : nums)
        {
            n = std::strtol(std::string(p.ReadUntil<'.', '-'>()).c_str(), nullptr, 10);
            if (!p.TryRead('.'))
                break;
        }

        // A fourth number and on are ignored. The pre-release is everything after the first '-'
        p.ReadUntil<'-'>();
        const std::string_view pre = p.TryRead('-') ? v.substr(p.pos) : std::string_view();
        return std::make_pair(nums, pre);
    };

    const auto [na, pa] = split(a);
    const auto [nb, pb] = split(b);
    if (na != nb)
        return na < nb ? -1 : 1;
    if (pa.empty() || pb.empty())
        return pa.empty() == pb.empty() ? 0 : (pa.empty() ? 1 : -1);  // a release is newer than its pre-releases

    // Pre-release identifiers: numbers compare as numbers and sort before words
    Parser ppa(pa), ppb(pb);
    while (!ppa.IsEof() || !ppb.IsEof())
    {
        if (ppa.IsEof())
            return -1;
        if (ppb.IsEof())
            return 1;
        const std::string_view xa = ppa.ReadUntil<'.'>();
        const std::string_view xb = ppb.ReadUntil<'.'>();
        ppa.TryRead('.');
        ppb.TryRead('.');
        const bool da = std::ranges::all_of(xa, ::isdigit);
        const bool db = std::ranges::all_of(xb, ::isdigit);
        if (da && db && xa.size() != xb.size())
            return xa.size() < xb.size() ? -1 : 1;
        if (da != db)
            return da ? -1 : 1;
        if (xa != xb)
            return xa < xb ? -1 : 1;
    }
    return 0;
}

bool is_installable(const index_version_t& v, std::string* why_not)
{
    std::string why;
    if (v.abi != OSHOT_API_VERSION)
        why = fmt::format("it's built for plugin ABI {}, this oshot has ABI {}", v.abi, OSHOT_API_VERSION);

    else if (v.imgui != 0 && v.imgui != OSHOT_IMGUI_VERSION)
        why = fmt::format("it's built for Dear ImGui {}, this oshot uses {}", v.imgui, OSHOT_IMGUI_VERSION);

    else if (!v.min_oshot.empty() && compare_versions(VERSION, v.min_oshot) < 0)
        why = fmt::format("it needs oshot {} or newer", v.min_oshot);

    else if (!v.artifacts.contains(OSHOT_TARGET) && !v.source)
        why = fmt::format("there's no build for {} and no source to build it from", OSHOT_TARGET);

    if (why_not)
        *why_not = why;
    return why.empty();
}

const index_version_t* pick_version(const index_plugin_t& plugin, std::string* why_not)
{
    std::string first_reason;
    for (const index_version_t& v : plugin.versions)
    {
        if (v.yanked)
            continue;
        std::string why;
        if (is_installable(v, &why))
            return &v;
        if (first_reason.empty())
            first_reason = fmt::format("{} {}", v.version, why);
    }
    if (why_not)
        *why_not = first_reason.empty() ? "every version was withdrawn" : first_reason;
    return nullptr;
}

// ----- parsing -----
static std::vector<std::string> str_array(const toml::table& tbl, std::string_view key)
{
    std::vector<std::string> out;
    if (const toml::array* arr = tbl[key].as_array())
        for (const toml::node& el : *arr)
            if (const auto* s = el.as_string())
                out.push_back(s->get());
    return out;
}

static std::map<std::string, std::string> str_table(const toml::table& tbl, std::string_view key)
{
    std::map<std::string, std::string> out;
    if (const toml::table* t = tbl[key].as_table())
        for (const auto& [k, v] : *t)
            if (const auto* s = v.as_string(); s && !s->get().empty())
                out.emplace(k.str(), s->get());
    return out;
}

static std::string str(const toml::table& tbl, std::string_view key)
{
    if (const toml::node* node = tbl.get(key))
    {
        if (const auto* s = node->as_string())
            return s->get();
        if (const auto* d = node->as_date())
            return fmt::format("{:04}-{:02}-{:02}", d->get().year, d->get().month, d->get().day);
    }
    return {};
}

Result<plugin_index_t> parse_plugin_index(const std::string& path)
{
    TomlAPI toml;
    TRY_MSG(toml.LoadFile(path), "The plugin index is broken: {}");
    const toml::table& root = toml.GetTbl();

    const int64_t schema = root["schema"].value_or(int64_t(0));
    if (schema != INDEX_SCHEMA)
        return Err("The plugin index has format {}, this oshot reads format {}: update oshot", schema, INDEX_SCHEMA);

    plugin_index_t index;
    if (const auto* gen = root["generated"].as_date_time())
    {
        std::ostringstream ss;
        ss << gen->get();
        index.generated = ss.str();
    }

    const toml::table* plugins = root["plugins"].as_table();
    if (!plugins)
        return Ok(std::move(index));

    for (const auto& [id, node] : *plugins)
    {
        const toml::table* tbl = node.as_table();
        if (!tbl || !Manifest::IsValidID(id.str()))
            continue;  // skip what this oshot can't make sense of, instead of rejecting the whole index

        index_plugin_t p;
        p.id           = id.str();
        p.name         = str(*tbl, "name");
        p.description  = str(*tbl, "description");
        p.homepage     = str(*tbl, "homepage");
        p.license      = str(*tbl, "license");
        p.owners       = str_array(*tbl, "owners");
        p.authors      = str_array(*tbl, "authors");
        p.tags         = str_array(*tbl, "tags");
        p.capabilities = str_table(*tbl, "capabilities");
        if (p.name.empty())
            p.name = p.id;

        if (const toml::array* versions = (*tbl)["versions"].as_array())
        {
            for (const toml::node& vnode : *versions)
            {
                const toml::table* vt = vnode.as_table();
                if (!vt)
                    continue;

                index_version_t v;
                v.version       = str(*vt, "version");
                v.date          = str(*vt, "date");
                v.min_oshot     = str(*vt, "min-oshot");
                v.changes       = str(*vt, "changes");
                v.abi           = (*vt)["abi"].value_or(int64_t(0));
                v.imgui         = (*vt)["imgui"].value_or(int64_t(-1));
                v.yanked        = (*vt)["yanked"].value_or(false);
                v.yanked_reason = str(*vt, "yanked-reason");
                if (v.version.empty())
                    continue;

                if (const toml::table* src = (*vt)["source"].as_table())
                    v.source = index_source_t{ .git    = str(*src, "git"),
                                               .tag    = str(*src, "tag"),
                                               .commit = str(*src, "commit"),
                                               .subdir = str(*src, "subdir") };
                if (const toml::table* arts = (*vt)["artifacts"].as_table())
                    for (const auto& [target, anode] : *arts)
                        if (const toml::table* at = anode.as_table())
                            v.artifacts.emplace(
                                target.str(), index_artifact_t{ .url = str(*at, "url"), .sha256 = str(*at, "sha256") });

                p.versions.push_back(std::move(v));
            }
        }

        std::ranges::sort(p.versions,
                          [](const auto& a, const auto& b) { return compare_versions(a.version, b.version) > 0; });
        index.plugins.push_back(std::move(p));
    }

    std::ranges::sort(index.plugins, {}, &index_plugin_t::id);
    return Ok(std::move(index));
}

Result<plugin_index_t> fetch_plugin_index(const fs::path& cache_file, bool allow_cached, bool* stale)
{
    if (stale)
        *stale = false;

    std::error_code ec;
    fs::create_directories(cache_file.parent_path(), ec);
    const fs::path tmp = fs::path(cache_file).concat(".new");

    Result<> dl = download_file(plugin_index_url(), tmp);
    if (dl.ok())
    {
        Result<plugin_index_t> index = parse_plugin_index(tmp.string());
        if (index.ok())
        {
            fs::rename(tmp, cache_file, ec);
            return index;
        }
        fs::remove(tmp, ec);
        if (!allow_cached || !fs::exists(cache_file, ec))
            return index;
    }
    else if (!allow_cached || !fs::exists(cache_file, ec))
    {
        return Err(dl.error_v());
    }

    if (stale)
        *stale = true;
    return parse_plugin_index(cache_file.string());
}
