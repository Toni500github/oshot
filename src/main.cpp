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

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "cache.hpp"
#include "platform.hpp"

#if OSHOT_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
// shellapi.h needs windows.h first
#  include <process.h>
#  include <shellapi.h>

#  include "dylib.hpp"
#else
#  include <unistd.h>
#endif

#include "app.hpp"
#include "cli.hpp"
#include "clipboard.hpp"
#include "config.hpp"
#ifndef DISABLE_PLUGINS
#  include "plugin.hpp"
#endif
#include "fmt/format.h"
#include "i18n.hpp"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include "util.hpp"
#include "util/image.hpp"
#include "util/paths.hpp"

static int oshot_main(int argc, char* argv[]);  // NOLINT

#if OSHOT_WINDOWS
static std::string wide_to_utf8(const wchar_t* w)
{
    if (!w)
        return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out;
    out.resize(size_t(needed) - 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// A GUI-subsystem app has no console, so help/version/errors are shown as dialogs
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // Per-monitor DPI awareness before the first capture: SDL only sets it in
    // SDL_Init(), so the first screenshot would otherwise see scaled monitor sizes
    wchar_t system_dir[MAX_PATH]{};
    GetSystemDirectoryW(system_dir, MAX_PATH);
    try
    {
        // Windows 10 1703+, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        dylib::library user32(fs::path(system_dir) / "user32.dll");
        user32.get_function<BOOL(HANDLE)>("SetProcessDpiAwarenessContext")(reinterpret_cast<HANDLE>(intptr_t(-4)));
    }
    catch (const std::exception&)
    {
        try
        {
            // Windows 8.1+, PROCESS_PER_MONITOR_DPI_AWARE
            dylib::library shcore(fs::path(system_dir) / "shcore.dll");
            shcore.get_function<HRESULT(int)>("SetProcessDpiAwareness")(2);
        }
        catch (const std::exception&)
        {
            SetProcessDPIAware();
        }
    }

    // In a GUI-subsystem app stdout/stderr only exist when redirected (scripts
    // reading --extract-text). Otherwise write to the console oshot was
    // started from, if any, or to NUL, because writing to a missing stream
    // fails and fmt::print() throws
    const auto has_stream = [](DWORD id) {
        HANDLE h = GetStdHandle(id);
        return h != nullptr && h != INVALID_HANDLE_VALUE;
    };
    const bool has_out    = has_stream(STD_OUTPUT_HANDLE);
    const bool has_err    = has_stream(STD_ERROR_HANDLE);
    const bool to_console = (!has_out || !has_err) && AttachConsole(ATTACH_PARENT_PROCESS);
    FILE*      dummy      = nullptr;
    if (!has_out)
        freopen_s(&dummy, to_console ? "CONOUT$" : "NUL", "w", stdout);
    if (!has_err)
        freopen_s(&dummy, to_console ? "CONOUT$" : "NUL", "w", stderr);

    // UTF-8 argv instead of the ANSI code page one
    std::vector<std::string> args;
    int                      wargc = 0;
    if (wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc))
    {
        for (int i = 0; i < wargc; ++i)
            args.push_back(wide_to_utf8(wargv[i]));
        LocalFree(wargv);
    }
    if (args.empty())
        args.emplace_back("oshot");

    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    return oshot_main(int(args.size()), argv.data());
}
#else
// Not caught on purpose, since with the tray and worker threads running there's nothing to unwind to
int main(int argc, char* argv[])
{
    return oshot_main(argc, argv);
}
#endif

static int oshot_main(int argc, char* argv[])  // NOLINT
{
    // Runs on every return, after the captures. Left to the global destructors, g_ss_tool would
    // go after Tesseract's static caches (FreeBSD aborted on exit after an OCR, Linux printed
    // "LEAK!" warnings), and g_plugins after spdlog's registry, which a plugin's destroy() may still log to
    struct ExitCleanup
    {
        ~ExitCleanup()
        {
#ifndef DISABLE_PLUGINS
            unload_plugins();
#endif
            g_ss_tool.EndOcr();
        }
    } exit_cleanup;

    std::error_code ec;

    // Don't make it relative to the working directory, which is System32 when started at sign-in on Windows
    const fs::path& log_path = fs::temp_directory_path(ec) / fmt::format("oshot_{}.log", getpid());
    fs::create_directories(log_path.parent_path(), ec);
    std::shared_ptr<spdlog::sinks::basic_file_sink_mt> file;
    try
    {
        file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.string(), true);
    }
    catch (const std::exception& e)
    {
        fmt::println(stderr, "Failed to open log file {}: {}", log_path.string(), e.what());
    }

#if OSHOT_LINUX
    // AppRun prepends the bundled libs to LD_LIBRARY_PATH, and child processes (zenity, grim...) would load
    // them instead of the host's and fail on symbol versions. Restore the value AppRun saved
    const char* orig = std::getenv("APPIMAGE_ORIG_LD_LIBRARY_PATH");
    if (orig)
        setenv("LD_LIBRARY_PATH", orig, 1);
    else
        unsetenv("LD_LIBRARY_PATH");  // not running from AppImage, clear any stale value
#endif

    signal(SIGINT, on_quit_signal);
    signal(SIGTERM, on_quit_signal);

    auto console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto imgui   = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(500);  // keep last 500 lines
    std::vector<spdlog::sink_ptr> sinks = { console, imgui };
    if (file)
        sinks.push_back(file);

    auto logger = std::make_shared<spdlog::logger>("oshot_logger", sinks.begin(), sinks.end());
    // [2026-03-10 17:24:07.593] [DEBUG] <col>message</col>
    logger->set_pattern("[%Y-%m-%d %T.%e] [%l] %^%v%$");
    logger->flush_on(spdlog::level::trace);
    spdlog::set_default_logger(logger);

    // Registered only now that spdlog's registry exists. Exit handlers run in reverse order, so it
    // still exists in exit_handler_nc()
    atexit(exit_handler_nc);
    g_imgui_log_sink = imgui;

    const std::string& configDir      = get_config_dir().string();
    const std::string& cacheDir       = get_cache_dir().string();
    const std::string& configFile     = parse_config_path(argc, argv, configDir).string();
    const std::string& imgui_ini_path = configDir + DIR_SEP_STR "imgui.ini";

    g_cache  = std::make_unique<Cache>(cacheDir);
    g_config = std::make_unique<Config>(configFile, configDir);
    g_clipboard.SetSession(get_session_type());
    if (!parseargs(argc, argv, configFile))
        return EXIT_FAILURE;

    const bool no_overlay = g_config->Runtime.extract_text || g_config->Runtime.extract_barcode ||
                            (g_config->Runtime.selection && g_config->Runtime.instant_copy_save != SavingOp::kNone);
    if (g_config->Runtime.selection && !no_overlay)
    {
        spdlog::error("--selection needs --extract-text, --extract-barcode, --instant-copy or --instant-save");
        return EXIT_FAILURE;
    }

    // stdout is for the extracted text, and the trace/debug logs would clutter a terminal or script
    if (no_overlay)
        console->set_level(spdlog::level::info);

    g_config->LoadConfigFile(configFile);
    {
        fs::path p(g_config->GetThemeFilePath());
        if (fs::exists(p) && p.has_filename())
            g_config->LoadThemeFile(p.string());
    }
    i18n::init(g_config->File.language, configDir);

    spdlog::set_level(spdlog::level::debug);

#if OSHOT_FREEDESKTOP
    // Must run before SDL loads OpenGL (GLX) and before any thread is spawned,
    // since setenv() isn't thread-safe against getenv() in other threads
    constexpr const char* GLX_ENV = "__GLX_VENDOR_LIBRARY_NAME";
    if (!g_config->File.glx_vendor.empty() &&
        setenv(GLX_ENV, g_config->File.glx_vendor.c_str(), int(g_config->File.pref_conf_to_env)) != 0)
        spdlog::warn("Failed to set {}: {}", GLX_ENV, std::strerror(errno));

    const char* glx_env = std::getenv(GLX_ENV);
    spdlog::debug("GLX vendor library: {}", glx_env ? glx_env : "not set");
#endif

    logger->info("=== oshot starting ===");
    if (file)
        logger->info("Log file path: {}", file->filename());

    if (no_overlay)
        return run_without_overlay();

    // Do this before taking the tray lock. Any number of --gui overlays can run at
    // once, and an open one shouldn't keep a tray from starting
    if (g_config->Runtime.only_launch_gui)
        return run_capture();

    return run_tray();
}
