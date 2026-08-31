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

#ifndef _TOML_API_HPP_
#define _TOML_API_HPP_

#include <algorithm>
#include <fstream>
#include <type_traits>
#include <unordered_map>

#include "util.hpp"

#define TOML_HEADER_ONLY 0
#include "toml++/toml.hpp"

enum class ValueType
{
    kNone,
    String,
    Bool,
    Int
};

struct override_config_value_t
{
    ValueType   value_type   = ValueType::kNone;
    std::string string_value = "";
    bool        bool_value   = false;
    int         int_value    = 0;
};

class TomlAPI
{
public:
    TomlAPI() = default;
    TomlAPI(toml::table tbl) : m_tbl(std::move(tbl)) {}
    virtual ~TomlAPI() = default;

    template <typename T>
    void SetValue(const std::string_view key, const T& value)
    {
        SetValueImpl(BuildKey(key), value);
    }

    template <typename T>
    void SetValue(const std::string_view name, const std::string_view key, const T& value)
    {
        toml::table* t = m_tbl[name].as_table();
        if (t)
            t->insert_or_assign(key, value);
    }

    template <typename T>
    T GetValue(const std::string_view key, const T& fallback, bool dont_expand_var = false) const
    {
        return GetValueImpl<T>(BuildKey(key), fallback, dont_expand_var);
    }

    template <typename T>
    T GetValueFromTable(const std::string_view name,
                        const std::string_view key,
                        const T&               fallback,
                        bool                   dont_expand_var = false) const
    {
        return GetValueIntern(key, m_tbl[name][key].value<T>(), fallback, dont_expand_var);
    }

    /**
     * Load config file and parse every config variables
     * @param filename The config file path
     */
    Result<> LoadFile(const std::string& filename)
    {
        try
        {
            m_tbl = toml::parse_file(filename);
        }
        catch (const toml::parse_error& err)
        {
            return Err(
                "Parsing toml file '{}' failed:\n"
                "{}\n"
                "\t(error occurred at line {} column {})",
                filename,
                err.description(),
                err.source().begin.line,
                err.source().begin.column);
        }
        return Ok();
    }

    bool SaveFile(const std::string& tofile)
    {
        std::stringstream ss;
        ss << m_tbl;
        return SaveFile(ss.str(), tofile);
    }

    // https://github.com/hyprwm/Hyprland/blob/2d2a5bebff72c73cd27db3b9e954b8fa2a7623e8/hyprpm/src/core/DataState.cpp#L24
    bool SaveFile(const std::string& str, const std::string& to)
    {
        // create temp file in a safe temp root
        const fs::path temp_state = (fs::temp_directory_path() / ".temp-cache");
        std::ofstream  of(temp_state, std::ios::trunc);
        if (!of.good())
            return false;

        of << str;
        of.close();

        return fs::copy_file(temp_state, to, fs::copy_options::overwrite_existing);
    }

    static std::string EscapeString(const std::string& s)
    {
        std::ostringstream oss;
        oss << toml::value<std::string>(s);  // includes surrounding quotes, fully escaped

        // remove quotes
        std::string str(oss.str());
        str.pop_back();
        str.erase(0, 1);

        // escape Windows path separators
        // aka. fucking backslashes because they need to be
        // backward compatible ofc
        for (size_t i = 0; i < str.size(); ++i)
        {
            if (str[i] == '\\')
            {
                str.insert(i + 1, 1, '\\');
                ++i;  // skip the backslash we just inserted
            }
        }
        return str;
    }

    // Ensures a sub-table exists for a given key. Returns a reference to the sub-table.
    static toml::table& EnsureTable(toml::table& parent, const std::string_view key)
    {
        if (toml::node* node = parent[key].node())
            if (toml::table* tbl = node->as_table())
                return *tbl;

        auto [it, inserted] = parent.insert(key, toml::table{});
        return *it->second.as_table();
    }

    toml::table& EnsureTable(const std::string_view key) { return EnsureTable(m_tbl, key); }

    // Converts a vector of strings to a toml::array
    static toml::array VectorToArray(const std::vector<std::string>& vec)
    {
        toml::array ret;
        for (const std::string& str : vec)
            ret.push_back(str);
        return ret;
    }

    /**
     * Override a config value from --override
     * @param str The value to override.
     *            Must have a '=' for separating the name and value to override.
     *            NO spaces between
     */
    void OverrideOption(const std::string& opt)
    {
        const size_t pos = opt.find('=');
        if (pos == std::string::npos)
            die("Option to override '{}' doesn't have an equal sign '=' for separating name and value\n"
                "See --help for more information",
                opt);

        std::string        name{ opt.substr(0, pos) };
        const std::string& value = opt.substr(pos + 1);

        // usually the user finds incovinient to write "default.foo"
        // for general config options
        if (name.find('.') == name.npos)
            name.insert(0, "default.");

        if (value == "true")
            m_overrides[name] = { .value_type = ValueType::Bool, .bool_value = true };
        else if (value == "false")
            m_overrides[name] = { .value_type = ValueType::Bool, .bool_value = false };
        else if ((value[0] == '"' && value.back() == '"') || (value[0] == '\'' && value.back() == '\''))
            m_overrides[name] = { .value_type = ValueType::String, .string_value = value.substr(1, value.size() - 2) };
        else if (std::ranges::all_of(value, ::isdigit))
            m_overrides[name] = { .value_type = ValueType::Int, .int_value = std::stoi(value) };
        else
            die("looks like override value '{}' from '{}' is neither a bool, int or string value", value, name);
    }

    /**
     * Override a config value from --override
     * @param key The value name to override.
     *            Must have a '=' for separating the name and value to override.
     *            NO spaces between
     * @param value The value that will overwrite
     */
    template <typename T>
    void OverrideOption(const std::string& key, const T& value)
    {
        override_config_value_t o;
        if constexpr (std::is_same_v<T, bool>)
        {
            o.value_type = ValueType::Bool;
            o.bool_value = value;
        }
        else if constexpr (std::is_convertible_v<T, std::string>)
        {
            o.value_type   = ValueType::String;
            o.string_value = value;
        }
        else if constexpr (std::is_convertible_v<T, int>)
        {
            o.value_type = ValueType::Int;
            o.int_value  = value;
        }

        m_overrides[key] = std::move(o);
    }

    std::vector<std::string> GetValueArrayStr(const std::string_view          value,
                                              const std::vector<std::string>& fallback) const
    {
        return GetValueArrayStrIntern(m_tbl.at_path(value).as_array(), fallback);
    }

    std::vector<std::string> GetValueArrayStr(const std::string_view          name,
                                              const std::string_view          key,
                                              const std::vector<std::string>& fallback) const
    {
        return GetValueArrayStrIntern(m_tbl[name][key].as_array(), fallback);
    }

    toml::table&       GetTbl() { return m_tbl; }
    const toml::table& GetTbl() const { return m_tbl; }
    const toml::array* GetValueArray(const std::string_view value) const { return m_tbl.at_path(value).as_array(); }

protected:
    virtual std::string BuildKey(const std::string_view key) const { return std::string(key); }

    toml::table m_tbl;

    std::unordered_map<std::string, override_config_value_t> m_overrides;

    /**
     * Set value of a config variables
     * @param path The config variable "path" (e.g "cache.source-path")
     */
    template <typename T>
    void SetValueImpl(const std::string_view key, const T& value)
    {
        toml::table* section = &m_tbl;
        size_t       start   = 0;

        for (;;)
        {
            size_t dot = key.find('.', start);
            if (dot == key.npos)
            {
                section->insert_or_assign(key.substr(start), value);
                return;
            }

            const std::string_view part = key.substr(start, dot - start);
            auto*                  next = section->get(part);
            if (!next || !next->is_table())
            {
                section->insert_or_assign(part, toml::table{});
                next = section->get(part);
            }
            section = next->as_table();
            start   = dot + 1;
        }
    }

    /**
     * Get value of config variables
     * @param value The config variable "path" (e.g "config.source-path")
     * @param fallback Default value if couldn't retrive value
     */
    template <typename T>
    T GetValueImpl(const std::string_view value, const T& fallback, bool dont_expand_var = false) const
    {
        return GetValueIntern(value, m_tbl.at_path(value).value<T>(), fallback, dont_expand_var);
    }

private:
    std::vector<std::string> GetValueArrayStrIntern(const toml::array*              array,
                                                    const std::vector<std::string>& fallback) const
    {
        std::vector<std::string> ret;

        // https://stackoverflow.com/a/78266628
        if (const toml::array* array_it = array)
        {
            ret.reserve(array_it->size());
            array_it->for_each([&](auto&& el) {
                if (const toml::value<std::string>* str_elem = el.as_string())
                    ret.push_back((*str_elem)->data());
            });

            return ret;
        }
        else
        {
            return fallback;
        }
    }

    /**
     * Get value of config variables
     * @param value The config variable "path" (e.g "config.source-path")
     * @param fallback Default value if couldn't retrive value
     */
    template <typename T>
    T GetValueIntern(const std::string_view  key,
                     const std::optional<T>& value,
                     const T&                fallback,
                     bool                    dont_expand_var = false) const
    {
        const auto& overridePos = m_overrides.find(std::string(key));

        if (overridePos != m_overrides.end())
        {
            const auto& ov = overridePos->second;
            if constexpr (std::is_same<T, bool>())
                if (ov.value_type == ValueType::Bool)
                    return ov.bool_value;
            if constexpr (std::is_same<T, std::string>())
                if (ov.value_type == ValueType::String)
                    return ov.string_value;
            if constexpr (std::is_same<T, int>())
                if (ov.value_type == ValueType::Int)
                    return ov.int_value;
        }

        if constexpr (toml::is_string<T>)
            if (!dont_expand_var)
                return value ? expand_var(value.value()) : expand_var(fallback);
            else
                return value ? value.value() : fallback;
        else
            return value.value_or(fallback);
    }
};

#endif  // !_TOML_API_HPP_
