#ifndef _CACHE_HPP_
#define _CACHE_HPP_

#include <string>
#include <unordered_map>

#define TOML_HEADER_ONLY 0
#include "toml++/toml.hpp"
#include "util.hpp"

enum class CacheEntry
{
    AnnColor,
    ImgSavePath,
    COUNT
};

class Cache
{
public:
    Cache(const std::string& cache_dir);
    ~Cache();

    Result<> LoadCacheFile();

    const std::string& GetCacheDirPath() const { return m_cache_dir_path; }

    /**
     * Get value of a cache variables
     * @param value The cache variable "path" (e.g "cache.source-path")
     * @param fallback Default value if couldn't retrive value
     */
    template <typename T>
    T GetValue(CacheEntry e, const T& fallback, bool dont_expand_var = false)
    {
        const std::string&      key = m_cache_entries.at(e);
        const std::optional<T>& ret = m_tbl["cache"][key].value<T>();
        if constexpr (toml::is_string<T>)
            if (!dont_expand_var)
                return ret ? expand_var(ret.value()) : expand_var(fallback);
            else
                return ret ? ret.value() : fallback;
        else
            return ret.value_or(fallback);
    }

    /**
     * Set value of a cache variables
     * @param path The cache variable "path" (e.g "cache.source-path")
     */
    template <typename T>
    void SetValue(CacheEntry e, const T& value)
    {
        const std::string& key     = m_cache_entries.at(e);
        auto*              section = m_tbl["cache"].as_table();
        if (!section)
        {
            m_tbl.insert_or_assign("cache", toml::table{});
            section = m_tbl["cache"].as_table();
        }
        section->insert_or_assign(key, value);
    }

private:
    static constexpr const char* mk_file_path = "cache.toml";

    std::string m_cache_dir_path;
    toml::table m_tbl;

    std::unordered_map<CacheEntry, std::string> m_cache_entries = {
        { CacheEntry::AnnColor, "default-color-picker-color" },
        { CacheEntry::ImgSavePath, "last-saved-dir" },
    };
};

extern std::unique_ptr<Cache> g_cache;

#endif  // !_CACHE_HPP_
