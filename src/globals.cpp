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

#include "cache.hpp"
#include "clipboard.hpp"
#include "config.hpp"
#ifndef DISABLE_PLUGINS
#  include "plugin.hpp"
#  include "state_manager.hh"
#endif
#include "screenshot_tool.hpp"
#include "util.hpp"

// Extern variables declariaions
std::deque<std::string> g_dropped_paths;
std::unique_ptr<Config> g_config;
std::unique_ptr<Cache>  g_cache;
bool                    g_is_systray = false;
int                     g_scr_w{}, g_scr_h{};
Clipboard               g_clipboard(SessionType::Unknown);

std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_imgui_log_sink;

#ifndef DISABLE_PLUGINS
static StateManager _s;
ScreenshotTool      g_ss_tool(std::move(_s));

std::unordered_map<std::string, plugin_runtime_t> g_plugins;
plugin_runtime_t*                                 g_current_plugin;
#else
ScreenshotTool g_ss_tool;
#endif
