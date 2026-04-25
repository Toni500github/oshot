#ifndef _CACHE_HPP_
#define _CACHE_HPP_

#include <string>
#include <unordered_map>

#define TOML_HEADER_ONLY 0
#include "toml++/toml.hpp"
#include "util.hpp"

enum class CacheFilesEnum
{
    Colors,
    COUNT
};

struct cache_entry_t
{
    const std::string file_path;
    const std::string toml_path;
    toml::table       tbl = {};
};

extern std::unordered_map<CacheFilesEnum, cache_entry_t> g_cache_files;

class Cache
{
public:
    Cache(const std::string& cache_dir);
    ~Cache();

    Result<> LoadCacheFile(cache_entry_t& entry);
    void     GenerateCacheFile(const cache_entry_t& entry);

    const std::string& GetCacheDirPath() const { return m_cache_dir_path; }

    /**
     * Get value of a cache variables
     * @param value The cache variable "path" (e.g "cache.source-path")
     * @param fallback Default value if couldn't retrive value
     */
    template <typename T>
    static T GetValue(const cache_entry_t& entry, const T& fallback, bool dont_expand_var = false)
    {
        const std::optional<T>& ret = entry.tbl.at_path(entry.toml_path).value<T>();
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
    static void SetValue(cache_entry_t& entry, const T& value)
    {
        std::string  path = entry.toml_path;
        toml::table* tbl  = &entry.tbl;

        while (true)
        {
            const auto& dot = path.find('.');
            if (dot == std::string_view::npos)
            {
                tbl->insert_or_assign(std::string(path), value);
                break;
            }

            const std::string& segment = path.substr(0, dot);
            path                       = path.substr(dot + 1);

            auto* node = tbl->get(segment);
            if (!node || !node->is_table())
            {
                tbl->insert_or_assign(segment, toml::table{});
                node = tbl->get(segment);
            }
            tbl = node->as_table();
        }
    }

private:
    std::string m_cache_dir_path;
};

extern std::unique_ptr<Cache> g_cache;

#endif  // !_CACHE_HPP_
