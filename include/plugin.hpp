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
#  include <map>
#  include <mutex>
#  include <thread>
#  include <utility>
#  include <vector>

#  ifndef OSHOT_HOST
#    define OSHOT_HOST
#  endif
#  include "../src/plugins/oshot_plugin.h"
#  include "dylib.hpp"
#  include "toml_api.hpp"

class StateManager;

// A loaded plugin. Lives in g_plugins from load until oshot exits; its address
// is the oshot_ctx_t the plugin gets, so it must never move.
struct plugin_runtime_t
{
    std::string           id;
    fs::path              dir;                    // ~/.config/oshot/plugins/<id>: library, config.toml, plugin data
    const oshot_plugin_t* plugin      = nullptr;  // static in the library, per the ABI contract
    void*                 state       = nullptr;  // owned by the plugin, released by plugin->destroy()
    bool                  initialized = false;
    dylib::library        lib;

    TomlAPI  config;  // the plugin's config.toml
    fs::path config_path;
    bool     config_dirty = false;

    std::mutex                                     tasks_mutex;  // run_on_ui_thread() queue
    std::vector<std::pair<void (*)(void*), void*>> tasks;

    plugin_runtime_t(std::string id, fs::path dir, const oshot_plugin_t* plugin, dylib::library lib)
        : id(std::move(id)), dir(std::move(dir)), plugin(plugin), lib(std::move(lib))
    {}

    ~plugin_runtime_t();

    plugin_runtime_t(const plugin_runtime_t&)            = delete;
    plugin_runtime_t& operator=(const plugin_runtime_t&) = delete;

    void SaveConfig();
    void RunUiTasks();
};

extern std::map<std::string, plugin_runtime_t> g_plugins;
extern const oshot_host_t                      g_oshot_host;

struct plugin_info_t
{
    std::string id;
    std::string name;
};

fs::path get_plugins_dir();

// Checks a descriptor against what this oshot supports (ABI, ImGui version, ID)
Result<> validate_plugin_descriptor(const oshot_plugin_t* plugin);

// Loads the library without initializing it and returns what its descriptor
// says, after validate_plugin_descriptor(). Used by oshotpm before installing.
Result<plugin_info_t> inspect_plugin_library(const fs::path& path);

// Loads every enabled plugin in state.toml, on the thread that will draw them
void load_plugins(const StateManager& state);

// Whether the caller is on the thread plugins were loaded on (their UI thread)
bool on_plugin_ui_thread();

#endif
