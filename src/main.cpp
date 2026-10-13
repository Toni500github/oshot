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

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "cache.hpp"
#include "platform.hpp"

#if !OSHOT_WINDOWS
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#if OSHOT_TOOL_ON_MAIN_THREAD
#  include <optional>

#  include "hotkey.hpp"
#  include "nvdialog/nvdialog_notification.h"
#endif

#include "clipboard.hpp"
#include "config.hpp"
#include "fmt/base.h"
#include "fmt/compile.h"
#include "fmt/format.h"
#include "getopt_port/getopt.h"
#include "nvdialog/nvdialog_error.h"
#include "oshot_png.h"
#include "screen_capture.hpp"
#include "screenshot_tool.hpp"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "switch_fnv1a.hpp"
#include "texts.hpp"
#include "tiny-process-library/process.hpp"
#include "tinyfiledialogs.h"
#include "tray.hpp"
#include "util.hpp"

using namespace Tray;
using namespace std::chrono_literals;

// Avoid dragging glfw headers
struct GLFWwindow;

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and
// compatibility with old VS compilers. To link with VS2010-era libraries, VS2015+ requires linking with
// legacy_stdio_definitions.lib, which we do using this pragma. Your own project should not be affected, as you are
// likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#  pragma comment(lib, "legacy_stdio_definitions")
#endif

// clang-format off
// https://cfengine.com/blog/2021/optional-arguments-with-getopt-long/
// because "--opt-arg arg" won't work
// but "--opt-arg=arg" will
#define OPTIONAL_ARGUMENT_IS_PRESENT \
    ((optarg == NULL && optind < argc && argv[optind][0] != '-') \
     ? (bool) (optarg = argv[optind++]) \
     : (optarg != NULL))
// clang-format on

std::error_code ec;

// Print the version and some other infos, then exit successfully
static void version()
{
    fmt::print(FMT_COMPILE("{}"), version_infos);
    fmt::print("\n");
    std::exit(EXIT_SUCCESS);
}

// Print the args help menu, then exit with code depending if it's from invalid or -h arg
static void help(bool invalid_opt = false)
{
    fmt::print(FMT_COMPILE("{}"), oshot_help);
    fmt::print("\n");
    std::exit(invalid_opt);
}

// clang-format off
// NOLINTBEGIN
// parseargs() but only for parsing the user config path trough args
// and so we can directly construct Config
static fs::path parse_config_path(int argc, char* argv[], const fs::path& configDir)
{
    int opt = 0;
    int option_index = 0;
    opterr = 0;
    const char *optstring = "-C:";
    static const struct option opts[] = {
        {"config", required_argument, 0, 'C'},
        {0,0,0,0}
    };

    while ((opt = getopt_long(argc, argv, optstring, opts, &option_index)) != -1)
    {
        switch (opt)
        {
            // skip errors or anything else
            case 0:
            case '?':
                break;

            case 'C':
                if (!fs::exists(optarg))
                    die("config file '{}' doesn't exist", optarg);
                return optarg;
        }
    }

    return configDir / "config.toml";
}

static bool parseargs(int argc, char* argv[], const fs::path& configFile)
{
    int opt = 0;
    int option_index = 0;
    opterr = 1; // re-enable since before we disabled for "invalid option" error
    const char *optstring = "-Vhtgd:C:f:O:";
    static const struct option opts[] = {
        {"version", no_argument,       0, 'V'},
        {"help",    no_argument,       0, 'h'},
        {"tray",    no_argument,       0, 't'},
        {"gui",     no_argument,       0, 'g'},
        {"delay",   required_argument, 0, 'd'},
        {"config",  required_argument, 0, 'C'},
        {"source",  required_argument, 0, 'f'},
        {"override",required_argument, 0, 'O'},

        {"instant-save", no_argument,       0, "instant-save"_fnv1a16},
        {"instant-copy", no_argument,       0, "instant-copy"_fnv1a16},
        {"gen-config",   optional_argument, 0, "gen-config"_fnv1a16},

        {0,0,0,0}
    };

    /* parse operation */
    optind = 1;
    while ((opt = getopt_long(argc, argv, optstring, opts, &option_index)) != -1)
    {
        switch (opt)
        {
            case 0:
            case 'C':
                break;
            case '?':
                help(EXIT_FAILURE); break;

            case 'V':
                version(); break;
            case 'h':
                help(); break;
            case 'f':
                g_config->Runtime.source_file = optarg; break;
            case 'O':
                g_config->OverrideOption(optarg); break;
            case 'd':
                g_config->OverrideOption("default.delay", std::atoi(optarg)); break;
            case 't':
                g_config->Runtime.only_launch_tray = true; break;
            case 'g':
                g_config->Runtime.only_launch_gui = true; break;

            case "instant-save"_fnv1a16:
                g_config->Runtime.instant_copy_save = SavingOp::File; break;
            case "instant-copy"_fnv1a16:
                g_config->Runtime.instant_copy_save = SavingOp::Clipboard; break;

            case "gen-config"_fnv1a16:
                if (OPTIONAL_ARGUMENT_IS_PRESENT)
                    g_config->GenerateConfig(optarg);
                else
                    g_config->GenerateConfig(configFile.string());
                exit(EXIT_SUCCESS);

            default:
                return false;
        }
    }

    return true;
}

// clang-format on
// NOLINTEND
static std::mutex              mtx;
static std::condition_variable cv;
static std::atomic<bool>       quit{ false };
static bool                    do_capture = false;
static std::string             pending_source;  // image to open instead of capturing, empty for a screenshot
static capture_result_t        pending_image;
static bool                    do_copy_image = false;
static std::atomic<bool>       capture_active{ false };  // a run_main_tool() is in progress, on any thread

bool app_quitting()
{
    return quit.load();
}

// Async-signal-safe: only raise the flag, the capture and tray loops act on
// it. Once the tray is up, Tray::SetQuitSignalCallback() takes over.
static void on_quit_signal(int)
{
    quit.store(true);
}

// Crash: restore the display mode, then re-raise so the OS still generates a core dump
static void on_crash_signal(int sig)
{
    extern_glfwTerminate();
    signal(sig, SIG_DFL);
    raise(sig);
}

// Quit from the tray menu or a signal (tray thread). A capture in progress
// sees app_quitting() and closes itself.
static void request_quit()
{
    {
        std::lock_guard lk(mtx);
        quit.store(true);
    }
    cv.notify_all();
    trayMaker.Exit();
}

static void exit_handler_nc()
{
    static std::atomic_flag ran = ATOMIC_FLAG_INIT;
    if (ran.test_and_set())
        return;

    request_quit();
#if !OSHOT_WINDOWS
    if (g_sock > 0)
        shutdown(g_sock, SHUT_RDWR);
#endif
    // Terminating GLFW under a running capture (e.g. die() from the worker
    // thread) would crash it instead of exiting
    if (!capture_active.load())
        extern_glfwTerminate();
    fs::remove(fs::temp_directory_path(ec) / fmt::format("oshot_{}.log", getpid()));
}

int run_main_tool();

static int run_capture()
{
    capture_active.store(true);
    const int ret = run_main_tool();
    capture_active.store(false);
    return ret;
}

void glfw_error_callback(int i_error, const char* description)
{
    error("GLFW Error {}: {}", i_error, description);
}

void glfw_drop_callback(GLFWwindow*, int count, const char** paths)
{
    for (int i = 0; i < count; ++i)
        g_dropped_paths.emplace_back(paths[i]);
}

void capture_worker()
{
    while (!quit.load())
    {
        // wait for command
        std::unique_lock lk(mtx);
        cv.wait(lk, [] { return quit.load() || do_capture || do_copy_image; });
        if (quit.load())
            break;

        if (do_copy_image)
        {
            do_copy_image        = false;
            capture_result_t img = std::move(pending_image);
            lk.unlock();
            MUST_OK(g_clipboard.CopyImage(img, g_config->File.image_out_type.second),
                    spdlog::error("{}", _r.error_v()));
            continue;
        }

        do_capture                    = false;
        g_config->Runtime.source_file = std::move(pending_source);
        pending_source.clear();
        // Set under the lock so capture_from_tray() can't queue another one in between
        capture_active.store(true);
        lk.unlock();
        run_main_tool();

        lk.lock();
        capture_active.store(false);
        lk.unlock();
        cv.notify_all();
    }
}

#if OSHOT_TOOL_ON_MAIN_THREAD
static void sync_hotkey();
#endif

// Open another overlay in a separate `oshot --gui` process, on top of the one
// already showing. This process has a single overlay (one GLFW window, ImGui
// context and g_ss_tool), and a new process also takes its screenshot with
// the current overlay in it, e.g. to report a visual bug in oshot itself.
static void spawn_gui_instance(const std::string& source)
{
    // Holding or mashing the hotkey shouldn't fork-bomb overlays
    static std::chrono::steady_clock::time_point last_spawn;
    const auto                                   now = std::chrono::steady_clock::now();
    if (now - last_spawn < 500ms)
        return;
    last_spawn = now;

    using string_type = TinyProcessLib::Process::string_type;
    // Same condition tiny-process uses for its string_type
    const auto to_proc_str = [](const fs::path& p) -> string_type {
#if defined(_WIN32) && defined(UNICODE)
        return p.wstring();
#else
        return p.string();
#endif
    };

    fs::path exe = get_exe_path();
#if OSHOT_LINUX
    // Inside an AppImage /proc/self/exe is the inner binary, which can't find
    // the bundled libs without AppRun's environment
    if (const char* appimage = std::getenv("APPIMAGE"))
        exe = appimage;
#endif
    if (exe.empty())
    {
        spdlog::error("Can't open another overlay: failed to get the oshot executable path");
        return;
    }

    std::vector<string_type> args = {
        to_proc_str(exe), to_proc_str("--gui"), to_proc_str("-C"), to_proc_str(g_config->GetConfigPath())
    };
    if (!source.empty())
    {
        args.push_back(to_proc_str("-f"));
        args.push_back(to_proc_str(source));
    }

    spdlog::info("A capture is already open, starting another overlay: {} --gui", exe.string());
#if OSHOT_WINDOWS
    // We got the hotkey/tray click, so we may take the foreground: pass that on,
    // otherwise the new overlay can open behind the current one
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    // Waited on from its own thread so it doesn't stay a zombie on unix
    auto proc = std::make_shared<TinyProcessLib::Process>(args, string_type{});
    std::thread([proc] {
        const int code = proc->get_exit_status();
        if (code != 0)
            spdlog::warn("Overlay process exited with code {}", code);
    }).detach();
}

// Start a capture from the tray (menu or global hotkey): a fresh screenshot,
// or the `source` image if not empty. Always sets the source, otherwise an
// image opened once would be reopened by every later capture.
static void capture_from_tray(const std::string& source = {})
{
#if OSHOT_TOOL_ON_MAIN_THREAD
    if (app_quitting())
        return;

    // The GLFW event pump inside run_main_tool() also dispatches the tray's
    // messages, so a second capture can't run in here: it would be nested
    // inside the first one. Stack another overlay process on top instead.
    if (capture_active.load())
    {
        spawn_gui_instance(source);
        return;
    }

    g_config->Runtime.source_file = source;
    run_capture();

    // The hotkey may have been changed from Preferences
    sync_hotkey();
#  if OSHOT_MACOS
    // glfwInit() finishing the app launch replaces the reopen handler
    Tray::SetReopenCallback([] { capture_from_tray(); });
#  endif
#else
    {
        std::lock_guard lk(mtx);
        // Already queued, about to start
        if (do_capture || quit.load())
            return;

        if (!capture_active.load())
        {
            pending_source = source;
            do_capture     = true;
            cv.notify_all();
            return;
        }
    }
    // Same as on Windows/macOS: stack another overlay process on top
    spawn_gui_instance(source);
#endif
}

#if OSHOT_TOOL_ON_MAIN_THREAD

// (Re-)register the global hotkey from the config if it changed
static void sync_hotkey()
{
#  if OSHOT_HAS_GLOBAL_HOTKEY
    static std::optional<std::string> applied;
    if (applied == g_config->File.hotkey)
        return;
    applied = g_config->File.hotkey;

    const Result<hotkey::hotkey_t>& hk  = hotkey::parse(*applied);
    const Result<>&                 res = [&]() -> Result<> {
        TRY(hk);
        return hotkey::enable(hk.get(), [] { capture_from_tray(); });
    }();
    if (res.ok())
    {
        spdlog::info("Global hotkey: {}", hotkey::status());
        return;
    }

    spdlog::warn("Global hotkey: {}", res.error_v());
    const std::string& body =
        fmt::format("{}\nPick another shortcut in Preferences, or set 'hotkey' in the config.", res.error_v());
    NvdNotification* notif = nvd_notification_new("oshot: hotkey not set", body.c_str(), NVD_NOTIFICATION_WARNING);
    if (notif)
    {
        nvd_send_notification(notif);
        nvd_delete_notification(notif);
    }
#  endif
}
#endif

#if OSHOT_WINDOWS && !defined(WINDOWS_CMD)
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

static bool wants_cli_output(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (!a)
            continue;

        if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0 || std::strcmp(a, "-V") == 0 ||
            std::strcmp(a, "--version") == 0 || std::strcmp(a, "-l") == 0 || std::strcmp(a, "--list") == 0)
            return true;
    }
    return false;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int       argc  = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::vector<std::string> argv_storage;
    std::vector<char*>       argv_ptrs;

    if (wargv && argc > 0)
    {
        argv_storage.reserve(argc);
        argv_ptrs.reserve(argc + 1);

        for (int i = 0; i < argc; ++i)
            argv_storage.push_back(wide_to_utf8(wargv[i]));

        for (auto& s : argv_storage)
            argv_ptrs.push_back(s.data());

        argv_ptrs.push_back(nullptr);

        LocalFree(wargv);
    }
    else
    {
        static char argv0[] = "oshot";
        argv_ptrs           = { argv0, nullptr };
        argc                = 1;
    }

    char** argv = argv_ptrs.data();

    // If we need to show CLI output, try to attach to parent console first.
    // This works when started from cmd.exe / powershell.
    if (wants_cli_output(argc, argv))
    {
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
        {
            // MSYS2/mintty often isn't a real Windows console -> attach fails.
            // Allocate a console so stdout/stderr have somewhere to go.
            AllocConsole();
        }

        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        freopen_s(&dummy, "CONIN$", "r", stdin);

        // Line-buffer so help/version prints immediately
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IOLBF, 0);
    }
    else
    {
        // Without a console, writing to stdout/stderr fails and fmt::print() throws
        FILE* dummy = nullptr;
        freopen_s(&dummy, "NUL", "w", stdout);
        freopen_s(&dummy, "NUL", "w", stderr);
    }
#else
int main(int argc, char* argv[])
{
#endif
    // Not relative to the working directory: when started at sign-in on Windows it's System32
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
    // AppRun prepends bundled libs to LD_LIBRARY_PATH so the AppImage is self-contained.
    // Child processes (zenity, grim, etc.) inherit it and resolve against the bundled
    // (older) libs instead of the host's, causing symbol version mismatches.
    // AppRun saves the original value here, restore it so that spawned system binaries
    // use the host libs.
    const char* orig = std::getenv("APPIMAGE_ORIG_LD_LIBRARY_PATH");
    if (orig)
        setenv("LD_LIBRARY_PATH", orig, 1);
    else
        unsetenv("LD_LIBRARY_PATH");  // not running from AppImage, clear any stale value
#endif

    atexit(exit_handler_nc);
    signal(SIGINT, on_quit_signal);
    signal(SIGTERM, on_quit_signal);
    signal(SIGABRT, on_crash_signal);
#if !OSHOT_WINDOWS
    signal(SIGSEGV, on_crash_signal);
#endif

    if (nvd_init() != 0)
    {
        fprintf(
            stderr, "Failed to initialize nvdialog: %s\n", nvd_string_to_cstr(nvd_stringify_error(nvd_get_error())));
        return -67;
    }

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto imgui   = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(500);  // keep last 500 lines
    std::vector<spdlog::sink_ptr> sinks = { console, imgui };
    if (file)
        sinks.push_back(file);
    spdlog::logger logger("oshot_logger", sinks.begin(), sinks.end());
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(logger));
    g_imgui_log_sink = imgui;

    // [2026-03-10 17:24:07.593] [DEBUG] <col>message</col>
    logger.set_pattern("[%Y-%m-%d %T.%e] [%l] %^%v%$");
    logger.flush_on(spdlog::level::trace);

    g_clipboard.SetSession(get_session_type());

    const std::string& configDir      = get_config_dir().string();
    const std::string& cacheDir       = get_cache_dir().string();
    const std::string& configFile     = parse_config_path(argc, argv, configDir).string();
    const std::string& imgui_ini_path = configDir + DIR_SEP_STR "imgui.ini";

    g_cache  = std::make_unique<Cache>(cacheDir);
    g_config = std::make_unique<Config>(configFile, configDir);
    if (!parseargs(argc, argv, configFile))
        return EXIT_FAILURE;

    g_config->LoadConfigFile(configFile);
    {
        fs::path p(g_config->File.theme_file_path);
        if (fs::exists(p) && p.has_filename())
            g_config->LoadThemeFile(p.string());
    }

    spdlog::set_level(spdlog::level::debug);

    logger.info("=== oshot starting ===");
    if (file)
        logger.info("Log file path: {}", file->filename());
    logger.flush();
    spdlog::flush_every(1s);

    // Before taking the tray lock: any number of --gui overlays can run at
    // once, and one shouldn't keep a tray from starting while it's open
    if (g_config->Runtime.only_launch_gui)
        return run_capture();

    const bool tray_lock_acquired = acquire_tray_lock();

    if (g_config->Runtime.only_launch_tray)
    {
        if (!tray_lock_acquired)
        {
            logger.info("oshot is already running in the system tray");
            return EXIT_SUCCESS;
        }

        g_is_systray = true;
    }
    else if (!tray_lock_acquired)
    {
        // Another instance owns the tray, just capture
        return run_capture();
    }

#if OSHOT_TOOL_ON_MAIN_THREAD
    // AppKit (macOS) needs the UI on the main thread, so the first capture runs
    // before the tray exists and later ones from the tray loop itself
    if (!g_config->Runtime.only_launch_tray)
        run_capture();
#else
    // Every capture runs on this one worker thread, so they can never overlap
    std::thread worker(capture_worker);
    if (!g_config->Runtime.only_launch_tray)
        capture_from_tray(g_config->Runtime.source_file);
#endif

    std::vector<TrayMenu*> menu;

#if OSHOT_WINDOWS
    // Absolute path: when started at sign-in the working directory is System32.
    // Fall back to the icon embedded in oshot.exe (see oshot.rc.in).
    fs::path tray_ico = get_exe_dir() / "oshot.ico";
    if (!fs::exists(tray_ico, ec))
        tray_ico = get_exe_path();
    TrayIcon tray = { "oshot.png", tray_ico.string(), "oshot", menu };
#else
    const fs::path& png_path = get_cache_dir() / "tray-icon.png";
    fs::create_directories(png_path.parent_path(), ec);
    std::ofstream out(png_path.string(), std::ios::binary | std::ios::out | std::ios::trunc);

    out.write(reinterpret_cast<const char*>(oshot_png), std::streamsize(oshot_png_len));
    out.close();
    TrayIcon tray = { .iconFilePng = png_path.string(), .iconFileIco = "oshot.ico", .tooltip = "oshot", .menu = menu };
#endif

    tray.menu.push_back(new TrayMenu{ .text        = "Capture",
                                      .isEnabled   = true,
                                      .isChecked   = false,
                                      .hasCheckbox = false,
                                      .onClicked   = [&](TrayMenu*) { capture_from_tray(); },
                                      .subMenu     = {} });

    tray.menu.push_back(new TrayMenu{ .text        = "Open Image",
                                      .isEnabled   = true,
                                      .isChecked   = false,
                                      .hasCheckbox = false,
                                      .onClicked =
                                          [&](TrayMenu*) {
                                              // Patterns are case-sensitive with zenity/kdialog
                                              const std::array<const char*, 8> filters = { "*.png", "*.jpeg", "*.jpg",
                                                                                           "*.bmp", "*.PNG",  "*.JPEG",
                                                                                           "*.JPG", "*.BMP" };

                                              const char* open_path =
                                                  tinyfd_openFileDialog("Open Image",
                                                                        "",              // default path
                                                                        filters.size(),  // number of filter patterns
                                                                        filters.data(),  // file filters
                                                                        "Images",        // filter description
                                                                        false            // allow multiple selections
                                                  );

                                              // Canceled the dialog
                                              if (!open_path)
                                                  return;

                                              capture_from_tray(open_path);
                                          },
                                      .subMenu = {} });

    tray.menu.push_back(new TrayMenu{ .text        = "Quit",
                                      .isEnabled   = true,
                                      .isChecked   = false,
                                      .hasCheckbox = false,
                                      .onClicked   = [&](TrayMenu*) { request_quit(); },
                                      .subMenu     = {} });

    if (trayMaker.Initialize(&tray))
    {
        Tray::SetQuitSignalCallback(request_quit);
#if OSHOT_TOOL_ON_MAIN_THREAD
        sync_hotkey();
#endif
#if OSHOT_MACOS
        Tray::SetReopenCallback([] { capture_from_tray(); });
#endif
        while (!app_quitting() && trayMaker.Loop(true))
        {
        }
    }
    else if (g_config->Runtime.only_launch_tray)
    {
        die("Systray initialization failed");
    }
    else
    {
        // e.g. no StatusNotifier host: still finish the capture the user asked for
        warn("Systray initialization failed, exiting after the current capture");
#if !OSHOT_TOOL_ON_MAIN_THREAD
        std::unique_lock lk(mtx);
        cv.wait(lk, [] { return quit.load() || (!do_capture && !capture_active.load()); });
#endif
    }

    // Quitted the tray
    request_quit();
#if !OSHOT_TOOL_ON_MAIN_THREAD
    worker.join();
#endif
    // Not in exit_handler_nc(): GLFW must be terminated on the main thread, with no capture running
    extern_glfwTerminate();

    return EXIT_SUCCESS;
}
