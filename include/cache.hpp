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

#ifndef _CACHE_HPP_
#define _CACHE_HPP_

#include <array>
#include <string>

#include "toml_api.hpp"
#include "util.hpp"

// util.hpp
std::string expand_var(std::string ret);

enum class CacheEntry
{
    AnnColor,
    ImgSavePath,
    COUNT
};

class Cache : public TomlAPI
{
public:
    Cache(const std::string& cache_dir);
    ~Cache();

    Result<> LoadCacheFile();

    const std::string& GetCacheDirPath() const { return m_cache_dir_path; }

    using TomlAPI::GetValue;
    using TomlAPI::SetValue;

    // CacheEntry convenience overloads are the ONLY thing Cache needs to add now
    template <typename T>
    T GetValue(CacheEntry e, const T& fallback, bool dont_expand_var = false)
    {
        return GetValue<T>(mk_cache_entries.at(idx(e)), fallback, dont_expand_var);
    }

    template <typename T>
    void SetValue(CacheEntry e, const T& value)
    {
        SetValue<T>(mk_cache_entries.at(idx(e)), value);
    }

protected:
    std::string BuildKey(const std::string_view key) const override { return fmt::format("cache.{}", key); }

private:
    void                         CreateFile();
    static constexpr const char* mk_file_path = "cache.toml";

    std::string m_cache_dir_path;

    static constexpr std::array<std::string_view, idx(CacheEntry::COUNT)> mk_cache_entries = {
        "default-color-picker-color",
        "last-saved-dir"
    };
};

extern std::unique_ptr<Cache> g_cache;

#endif  // !_CACHE_HPP_
