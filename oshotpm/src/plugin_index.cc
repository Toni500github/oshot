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
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "fmt/format.h"
#include "manifest.hh"
#include "plugin.hpp"
#include "tiny-process-library/process.hpp"
#include "toml_api.hpp"

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

// ----- SHA-256 (FIPS 180-4) -----
namespace
{
class Sha256
{
public:
    void Update(const uint8_t* data, size_t len)
    {
        m_total += len;
        while (len > 0)
        {
            const size_t n = std::min(len, 64 - m_used);
            std::copy_n(data, n, m_block.begin() + static_cast<std::ptrdiff_t>(m_used));
            m_used += n;
            data += n;
            len -= n;
            if (m_used == 64)
            {
                Transform();
                m_used = 0;
            }
        }
    }

    std::string HexDigest()
    {
        const uint64_t bits = m_total * 8;
        const uint8_t  pad  = 0x80;
        Update(&pad, 1);
        const uint8_t zero = 0;
        while (m_used != 56)
            Update(&zero, 1);
        for (int i = 7; i >= 0; --i)
        {
            const auto byte = static_cast<uint8_t>(bits >> (i * 8));
            Update(&byte, 1);
        }

        std::string hex;
        for (const uint32_t h : m_state)
            hex += fmt::format("{:08x}", h);
        return hex;
    }

private:
    static constexpr std::array<uint32_t, 64> K = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void Transform()
    {
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i)
            w[i] = uint32_t(m_block[i * 4]) << 24 | uint32_t(m_block[i * 4 + 1]) << 16 |
                   uint32_t(m_block[i * 4 + 2]) << 8 | uint32_t(m_block[i * 4 + 3]);
        for (size_t i = 16; i < 64; ++i)
        {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i]              = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
        for (size_t i = 0; i < 64; ++i)
        {
            const uint32_t s1  = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch  = (e & f) ^ (~e & g);
            const uint32_t t1  = h + s1 + ch + K[i] + w[i];
            const uint32_t s0  = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2  = s0 + maj;
            h                  = g;
            g                  = f;
            f                  = e;
            e                  = d + t1;
            d                  = c;
            c                  = b;
            b                  = a;
            a                  = t1 + t2;
        }
        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<uint32_t, 8> m_state = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::array<uint8_t, 64> m_block{};
    size_t                  m_used  = 0;
    uint64_t                m_total = 0;
};
}  // namespace

Result<std::string> sha256_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Err("Can't read '{}'", path.string());

    Sha256                    sha;
    std::array<char, 1 << 16> buf{};
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0)
        sha.Update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(in.gcount()));
    return Ok(sha.HexDigest());
}

// ----- versions -----
int compare_versions(std::string_view a, std::string_view b)
{
    const auto split = [](std::string_view v) {
        const size_t           dash = v.find('-');
        const std::string_view core = v.substr(0, dash);
        const std::string_view pre  = dash == v.npos ? std::string_view() : v.substr(dash + 1);
        std::array<long, 3>    nums{};
        size_t                 i = 0, start = 0;
        for (size_t pos = 0; pos <= core.size() && i < 3; ++pos)
        {
            if (pos == core.size() || core[pos] == '.')
            {
                nums[i++] = std::strtol(std::string(core.substr(start, pos - start)).c_str(), nullptr, 10);
                start     = pos + 1;
            }
        }
        return std::make_pair(nums, pre);
    };

    const auto [na, pa] = split(a);
    const auto [nb, pb] = split(b);
    if (na != nb)
        return na < nb ? -1 : 1;
    if (pa.empty() || pb.empty())
        return pa.empty() == pb.empty() ? 0 : (pa.empty() ? 1 : -1);  // a release is newer than its pre-releases

    // Pre-release identifiers: numbers compare as numbers and sort before words
    size_t ia = 0, ib = 0;
    while (ia < pa.size() || ib < pb.size())
    {
        if (ia >= pa.size())
            return -1;
        if (ib >= pb.size())
            return 1;
        const size_t           ea = std::min(pa.find('.', ia), pa.size());
        const size_t           eb = std::min(pb.find('.', ib), pb.size());
        const std::string_view xa = pa.substr(ia, ea - ia);
        const std::string_view xb = pb.substr(ib, eb - ib);
        const bool             da = std::ranges::all_of(xa, ::isdigit);
        const bool             db = std::ranges::all_of(xb, ::isdigit);
        if (da && db && xa.size() != xb.size())
            return xa.size() < xb.size() ? -1 : 1;
        if (da != db)
            return da ? -1 : 1;
        if (xa != xb)
            return xa < xb ? -1 : 1;
        ia = ea + 1;
        ib = eb + 1;
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
        p.id          = id.str();
        p.name        = str(*tbl, "name");
        p.description = str(*tbl, "description");
        p.homepage    = str(*tbl, "homepage");
        p.license     = str(*tbl, "license");
        p.owners      = str_array(*tbl, "owners");
        p.authors     = str_array(*tbl, "authors");
        p.tags        = str_array(*tbl, "tags");
        p.network     = (*tbl)["network"].value_or(false);
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
