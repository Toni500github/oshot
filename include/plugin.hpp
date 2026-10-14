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

#pragma once

#ifndef DISABLE_PLUGINS
#  include <unordered_map>

#  include "../oshotpm/include/manifest.hh"
#  include "../src/plugins/oshot_plugin.h"
#  include "dylib.hpp"
#  include "toml_api.hpp"

struct plugin_runtime_t;

extern std::unordered_map<std::string, plugin_runtime_t> g_plugins;
extern plugin_runtime_t*                                 g_current_plugin;  // plain global, see above

struct ScopedActivePlugin
{
    plugin_runtime_t* prev;
    explicit ScopedActivePlugin(plugin_runtime_t* rt) : prev(g_current_plugin) { g_current_plugin = rt; }
    ~ScopedActivePlugin() { g_current_plugin = prev; }
};

struct plugin_runtime_t
{
    std::string id;             // must be validated
    std::string config_prefix;  // cached "plugins.<id>."
    fs::path    data_dir;
    fs::path    plugin_path;

    oshot_plugin_t* plugin  = nullptr;  // non-owning and static, process-lifetime per ABI contract
    void*           state   = nullptr;  // owned by the plugin; released via plugin->destroy()
    const bool      enabled = false;    // set once, never changed
    dylib::library  lib;

    TomlAPI  config;       // this plugin's own config, loaded from
    fs::path config_path;  // ~/.config/oshot/plugins/<id>/config.toml

    plugin_runtime_t(std::string     id,
                     std::string     config_prefix,
                     fs::path        data_dir,
                     fs::path        plugin_path,
                     oshot_plugin_t* plugin,
                     void*           state,
                     bool            enabled,
                     dylib::library  lib,
                     const TomlAPI&  config,
                     fs::path        config_path)
        : id(std::move(id)),
          config_prefix(std::move(config_prefix)),
          data_dir(std::move(data_dir)),
          plugin_path(std::move(plugin_path)),
          plugin(plugin),
          state(state),
          enabled(enabled),
          lib(std::move(lib)),
          config(config),
          config_path(std::move(config_path))
    {}

    ~plugin_runtime_t()
    {
        if (plugin && plugin->destroy && state)
        {
            ScopedActivePlugin _(this);  // destroy() may still call oshot_log/config_get_*
            plugin->destroy(state);
        }
    }

    plugin_runtime_t(const plugin_runtime_t&)            = delete;
    plugin_runtime_t& operator=(const plugin_runtime_t&) = delete;
    plugin_runtime_t(plugin_runtime_t&&)                 = default;
};

void load_plugins(const std::vector<manifest_t>& repos);

#endif
