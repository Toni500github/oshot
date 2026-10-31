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
#  include <atomic>
#  include <chrono>
#  include <condition_variable>
#  include <cstddef>
#  include <functional>
#  include <map>
#  include <memory>
#  include <mutex>
#  include <span>
#  include <string>
#  include <thread>
#  include <utility>
#  include <variant>
#  include <vector>

#  ifndef OSHOT_HOST
#    define OSHOT_HOST
#  endif
#  include "../src/plugins/oshot_plugin.h"
#  include "dylib.hpp"
#  include "text_extraction.hpp"
#  include "toml_api.hpp"

class StateManager;

// Whether a plugin's descriptor has `field`: one built with an older
// oshot_plugin.h is shorter and ends before the fields added since
#  define OSHOT_PLUGIN_HAS(plugin, field) \
      (offsetof(oshot_plugin_t, field) + sizeof(((oshot_plugin_t*)0)->field) <= (plugin)->struct_size)

// The value of a declarative setting: CHOICE settings hold the chosen string
using setting_value_t = std::variant<bool, int64_t, double, std::string>;

// Mistakes in a plugin's UI code, each logged only once per plugin
enum class UiWarning : uint32_t  // NOLINT
{
    OutsideRender = 1 << 0,
    OffThread     = 1 << 1,
    OpenSection   = 1 << 2,
    ExtraEnd      = 1 << 3,
    ImGuiStack    = 1 << 4,
    SlowTransform = 1 << 5,
    BadTransform  = 1 << 6,
    OutsideDraw   = 1 << 7,
    BadOcrWords   = 1 << 8,
};

// An action from register_action()
struct plugin_action_t
{
    // The button's image, RGBA, icon_w * icon_h * 4 bytes; empty for none
    std::vector<uint8_t> icon;

    std::string   label;
    void          (*fn)(void* user, int32_t action) = nullptr;
    void*         user                              = nullptr;
    int32_t       id                                = 0;
    int32_t       icon_w                            = 0;
    int32_t       icon_h                            = 0;
    oshot_place_t place                             = OSHOT_PLACE_TOOLBAR;
};

// What report_progress() or report_result() last said about an action, shown in the overlay
struct plugin_report_t
{
    // The progress text, or the result's message
    std::string text;

    // When the result came, for hiding it after a while
    std::chrono::steady_clock::time_point finished;

    std::string copyable;
    float       fraction = -1.0f;  // negative: unknown
    int32_t     action   = 0;      // 0: not tied to an action
    bool        done     = false;  // a result, not progress
    bool        ok       = false;
};

// A request_final_image() call waiting for the frame to be read back
struct plugin_image_request_t
{
    void  (*on_ready)(void* user, oshot_capture_t image);
    void* user;
};

// An OCR engine from register_ocr_provider(), with oshot's own copies of its strings
struct plugin_ocr_provider_t
{
    std::vector<std::string> languages;

    // The ID within its plugin, and what the engine picker shows
    std::string id;
    std::string name;

    oshot_err_t (*recognize)(void*, const oshot_ocr_request_t*, oshot_ocr_output_t*, oshot_str_t*) = nullptr;

    void*    user  = nullptr;
    uint32_t flags = 0;
};

// A child process run_process() started, see oshot_plugins.cc
struct plugin_process_t;

// A plugin's run_process() children, shared with the threads waiting for
// them, which only reach the plugin through here (they can outlive it)
struct plugin_processes_t
{
    std::mutex mutex;

    // Signaled when a process exits
    std::condition_variable cv;

    // The on_exit calls of the processes that exited, run by RunUiTasks()
    std::vector<std::function<void()>> exited;

    // Set when the plugin unloads. No more processes start after that, and on_exit isn't called
    bool closed = false;

    std::map<int64_t, std::shared_ptr<plugin_process_t>> running;
    int64_t                                              next_id = 1;
};

// A loaded plugin. Lives in g_plugins from load until oshot exits; its address
// is the oshot_ctx_t the plugin gets, so it must never move.
struct plugin_runtime_t
{
    // Declared first so it's destroyed last: the library is unloaded after everything else here
    dylib::library lib;

    // ~/.config/oshot/plugins/<id>: library, config.toml, plugin data
    fs::path dir;

    // Static in the library, per the ABI contract
    const oshot_plugin_t* plugin = nullptr;

    // Owned by the plugin, released by plugin->destroy()
    void* state = nullptr;

    // The plugin's config.toml, written to config_path by SaveConfig() when config_dirty
    TomlAPI config;

    // run_on_ui_thread() queue, emptied by RunUiTasks()
    std::mutex                         tasks_mutex;
    std::vector<std::function<void()>> tasks;

    std::shared_ptr<plugin_processes_t> processes = std::make_shared<plugin_processes_t>();

    // plugin->settings as they're in config.toml, and as Preferences shows them until saved
    std::vector<setting_value_t> settings_saved;
    std::vector<setting_value_t> settings_editing;

    // True while render() or render_preferences() runs, the only time the ui_* functions work
    bool in_ui = false;

    // True while render_overlay() runs, the only time the draw_* functions work
    bool in_overlay = false;

    // The ui_begin_section() calls not ended yet, and whether each one is open
    std::vector<bool> sections;

    // UiWarning bits already logged; atomic because the ui_* functions can be called from any thread
    std::atomic<uint32_t> warned = 0;

    // register_action()'s actions, in the order they were added. UI thread only
    std::vector<plugin_action_t> actions;

    // The ID the next register_action() returns. Atomic since report_*() check IDs against it from any thread
    std::atomic<int32_t> next_action_id = 1;

    // report_progress() and report_result(), one entry per action, written from any thread
    std::mutex                   reports_mutex;
    std::vector<plugin_report_t> reports;

    // request_final_image() calls waiting for DeliverFinalImage(). UI thread only
    std::vector<plugin_image_request_t> image_requests;

    // register_ocr_provider()'s engines, in the order they were added. UI thread only
    std::vector<plugin_ocr_provider_t> ocr_providers;

    std::string id;
    fs::path    config_path;
    bool        initialized  = false;
    bool        config_dirty = false;
    bool        in_capture   = false;  // between CaptureStart() and CaptureEnd()

    plugin_runtime_t(std::string id, fs::path dir, const oshot_plugin_t* plugin, dylib::library lib)
        : lib(std::move(lib)), dir(std::move(dir)), plugin(plugin), id(std::move(id))
    {}

    // Ends its capture, kills its processes, calls plugin->destroy() if it was initialized, then SaveConfig()
    ~plugin_runtime_t();

    plugin_runtime_t(const plugin_runtime_t&)            = delete;
    plugin_runtime_t& operator=(const plugin_runtime_t&) = delete;

    // Runs the queued run_on_ui_thread() tasks and on_exit calls. Call it on the UI thread
    void RunUiTasks();

    /* Call plugin->on_capture_start(), if it has one */
    void CaptureStart();

    /**
     * Call plugin->on_capture_end() if CaptureStart() was called since the last
     * time, after answering the pending request_final_image() calls with no image
     * and dropping the reports
     **/
    void CaptureEnd();

    /**
     * Answer the pending request_final_image() calls, each with its own copy
     * @param image The annotated selection, or an empty one when there's none
     **/
    void DeliverFinalImage(const capture_result_t& image);

    /**
     * Store what report_progress() or report_result() said, replacing the
     * action's last report
     * @param report The new report
     **/
    void Report(plugin_report_t report);

    /**
     * Find an action by ID
     * @return The action, or nullptr
     **/
    [[nodiscard]] const plugin_action_t* FindAction(int32_t id) const;

    /**
     * Kill the processes run_process() started, wait a moment for them to
     * exit, and drop their on_exit calls. Implemented in oshot_plugins.cc
     **/
    void KillProcesses();

    /**
     * Call plugin->render() inside the plugin's ImGui ID scope, with the ui_*
     * functions enabled, and close what it left open. Call it inside a window
     **/
    void Render();

    /**
     * The same for plugin->render_preferences()
     * @return What render_preferences() returned: true while it has unsaved changes
     **/
    bool RenderPreferences();

    /**
     * The same for plugin->render_overlay(), with the draw_* functions enabled
     * instead. Call it while the overlay draws, if the plugin has one
     * @param info The selection and where the screenshot is drawn
     **/
    void RenderOverlay(const oshot_overlay_info_t& info);

    /**
     * Find one of its OCR engines by ID
     * @return The engine, or nullptr
     **/
    [[nodiscard]] const plugin_ocr_provider_t* FindOcrProvider(std::string_view provider_id) const;

    /**
     * Log a mistake in the plugin's UI code, the first time it happens
     * @param warning Which mistake
     * @param msg The message, after the plugin's ID
     **/
    void WarnOnce(UiWarning warning, std::string_view msg);

    /* The declarative settings, empty if the plugin has none or is too old for them */
    [[nodiscard]] std::span<const oshot_setting_t> Settings() const;

    /* Read the settings' values from config.toml into settings_saved and settings_editing */
    void LoadSettings();

    /* Write settings_editing into config.toml (on the next SaveConfig()) */
    void StoreSettings();

    void SaveConfig();
};

// A plugin's transform_text() in the chain, see arrange_text_transformers()
struct text_transformer_t
{
    plugin_runtime_t* rt;
    bool              enabled;
};

// One step of run_text_steps(): a transformer by its plugin's ID
struct text_step_t
{
    std::string                                id;
    std::function<bool(oshot_str_t, oshot_str_t*)> fn;
};

// What an `ocr-engine` value picks, see resolve_ocr_engine()
enum class OcrEngineKind : uint8_t
{
    Tesseract,
    Plugin,   // an engine a loaded plugin registered
    Missing,  // a plugin's engine that isn't there (the plugin is disabled, uninstalled or didn't register it)
};

// A scan by a plugin's OCR engine, running recognize() on its own thread
class OcrJob
{
public:
    /**
     * Start recognize() on a new thread
     * @param plugin_id The engine's plugin, for the log
     * @param provider The engine, copied
     * @param image The selection, copied
     * @param language What the request's language is set to
     **/
    OcrJob(std::string plugin_id, plugin_ocr_provider_t provider, capture_result_t image, std::string language);

    // Cancels it and waits for recognize() to return
    ~OcrJob();

    OcrJob(const OcrJob&)            = delete;
    OcrJob& operator=(const OcrJob&) = delete;

    /* Make the request's cancelled() return true. Any thread */
    void Cancel();

    /* Wait for recognize() to return. Only from the thread that made the job */
    void Wait();

    /* Whether recognize() returned and the result is ready. Any thread */
    [[nodiscard]] bool Done() const { return m_done; }

    /**
     * Take the result, once Done(). Its words are in the pixels of the image
     * the job got, and its engine field is set to the engine's name
     **/
    Result<ocr_result_t> TakeResult();

    // What the job runs, for unregister_ocr_provider()
    const std::string           plugin_id;
    const plugin_ocr_provider_t provider;

private:
    void Run(capture_result_t image, std::string language);

    // Written by the thread before m_done is set, read only after
    Result<ocr_result_t> m_result{ Err("not done") };

    std::atomic<bool> m_cancelled = false;
    std::atomic<bool> m_done      = false;

    // Last, so it starts after the members above exist
    std::thread m_thread;
};

// What inspect_plugin_library() read from a plugin's descriptor
struct plugin_info_t
{
    std::string id;
    std::string name;
};

// The loaded plugins, by ID
extern std::map<std::string, plugin_runtime_t> g_plugins;

// The host function table every plugin gets in init()
extern const oshot_host_t g_oshot_host;

// The loaded plugins that have a transform_text(), in the order the user set
extern std::vector<text_transformer_t> g_text_transformers;

/**
 * Get the directory plugins are installed in
 * @return get_config_dir() / "plugins"
 **/
fs::path get_plugins_dir();

/**
 * Check a plugin descriptor against what this oshot supports (ABI, ImGui version, ID)
 * @param plugin The descriptor oshot_plugin_entry() returned
 **/
Result<> validate_plugin_descriptor(const oshot_plugin_t* plugin);

/**
 * Load a plugin library without initializing it, and read its descriptor after
 * validate_plugin_descriptor(). Used by oshotpm before installing
 * @param path The plugin library
 **/
Result<plugin_info_t> inspect_plugin_library(const fs::path& path);

/**
 * Load every enabled plugin in state.toml into g_plugins.
 * Call it on the thread that will draw them, which becomes their UI thread
 * @param state The installed plugins
 **/
void load_plugins(const StateManager& state);

/**
 * Unload every plugin. That means on_capture_end() if a capture is open, killing
 * their processes, then destroy(). Call it before main() returns, while the logger exists
 **/
void unload_plugins();

/* Check whether the caller is on the thread plugins were loaded on (their UI thread) */
bool on_plugin_ui_thread();

/**
 * Set whether an overlay is open, so report_result() knows to fall back to a notification
 * @param open True from a capture's start to its end
 **/
void set_plugin_overlay_open(bool open);

/* Check whether an overlay is open, from any thread */
bool plugin_overlay_open();

/**
 * Rebuild g_text_transformers from the loaded plugins and the order in state.toml
 * @param state The installed plugins, with their [text-transformers] order
 **/
void sort_text_transformers(const StateManager& state);

/**
 * Check an OCR engine passed to register_ocr_provider() before oshot copies it
 * @param provider What the plugin passed
 **/
Result<> validate_ocr_provider(const oshot_ocr_provider_t* provider);

/**
 * Get the `ocr-engine` value for a plugin's OCR engine
 * @param plugin_id The plugin's ID
 * @param provider_id The engine's ID within the plugin
 * @return "<plugin id>/<provider id>"
 **/
std::string ocr_engine_key(std::string_view plugin_id, std::string_view provider_id);

/**
 * Tell what an `ocr-engine` value picks
 * @param engine The value, e.g. from the config
 * @param available ocr_engine_key() of every engine the loaded plugins registered
 * @return Tesseract for "tesseract", an empty or malformed value; Plugin if it's
 *         one of `available`; Missing otherwise
 **/
OcrEngineKind resolve_ocr_engine(std::string_view engine, std::span<const std::string> available);

/**
 * Find the plugin engine an `ocr-engine` value picks, among the loaded plugins
 * @param engine The value
 * @param rt Set to the engine's plugin, if found
 * @return The engine, or nullptr when it's Tesseract or missing
 **/
const plugin_ocr_provider_t* find_ocr_provider(std::string_view engine, plugin_runtime_t** rt = nullptr);

/**
 * Pick the language a plugin's OCR engine reads in
 * @param languages The engine's languages
 * @param picked The one picked in the text tools, if any
 * @param tesseract_model The Tesseract model in use, e.g. "eng"
 * @return `picked` if the engine has it, else `tesseract_model` if it has that,
 *         else its first language, or empty if it has none
 **/
std::string pick_ocr_language(std::span<const std::string> languages,
                              const std::string&           picked,
                              const std::string&           tesseract_model);

/* Get ocr_engine_key() of every engine the loaded plugins registered, in plugin then registration order */
std::vector<std::string> list_ocr_providers();

/**
 * Turn what an OCR engine's recognize() filled in into oshot's result, then
 * free it. Words with no box or text are skipped, boxes are clamped to the image,
 * an empty text is joined from the words and a confidence of -1 is their mean
 * @param out What recognize() filled in, empty afterwards
 * @param img_w, img_h The size of the image the engine read
 * @param prep The prepared image the engine read, to map its boxes back; nullptr if it read the selection as is
 * @param bad_words Set to true if some words were skipped
 **/
ocr_result_t ocr_result_from_output(oshot_ocr_output_t&   out,
                                    int                   img_w,
                                    int                   img_h,
                                    const ocr_prepared_t* prep,
                                    bool*                 bad_words);

/* Free what recognize() filled in, with the host's allocator, and empty it */
void free_ocr_output(oshot_ocr_output_t& out);

/**
 * Start a scan with a plugin's OCR engine. oshot keeps the job until it's done,
 * and unload_plugins() waits for it
 * @param rt The engine's plugin
 * @param provider The engine
 * @param image The selection
 * @param language The language picked for it, or empty
 **/
std::shared_ptr<OcrJob> start_ocr_job(const plugin_runtime_t&      rt,
                                      const plugin_ocr_provider_t& provider,
                                      capture_result_t             image,
                                      std::string                  language);

/**
 * Cancel the scans of a plugin's engine
 * @param plugin_id The plugin
 * @param provider_id The engine, or empty for all of the plugin's
 **/
void cancel_ocr_jobs(std::string_view plugin_id, std::string_view provider_id);

/* Forget the jobs that are done and no one waits for. Call it on the UI thread now and then */
void reap_ocr_jobs();

/**
 * Pass text through transformers one after another, freeing what each returned
 * @param text What the scan read
 * @param steps The transformers, in order
 * @param slow How long a step may take before on_slow is called
 * @param on_slow Called with a step that took longer than `slow`, and how long it took
 * @param on_bad Called with a step that returned true without valid text, which is then skipped
 * @return The text after the last step
 **/
std::string run_text_steps(std::string                                                          text,
                           std::span<const text_step_t>                                         steps,
                           std::chrono::milliseconds                                            slow,
                           const std::function<void(const text_step_t&, std::chrono::milliseconds)>& on_slow,
                           const std::function<void(const text_step_t&)>&                       on_bad);

/**
 * Run the enabled transformers of g_text_transformers over a scan's text, logging
 * the ones that are slow or broken once per plugin
 * @param text What the scan read
 * @param buffer_id OSHOT_TEXT_OCR or OSHOT_TEXT_BARCODE
 * @return The transformed text
 **/
std::string run_text_transformers(std::string text, const char* buffer_id);

#endif
