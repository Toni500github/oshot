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

#include "app.hpp"

#include <SDL3/SDL_init.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "platform.hpp"

#ifdef __GLIBC__
#  include <malloc.h>
#endif

#if OSHOT_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#if OSHOT_TOOL_ON_MAIN_THREAD
#  include <ranges>

#  include "hotkey.hpp"
#  include "nvdialog/nvdialog_notification.h"
#endif

#include "config.hpp"
#include "i18n.hpp"
#include "oshot_png.h"
#include "screenshot_tool.hpp"
#include "spdlog/spdlog.h"
#include "tiny-process-library/process.hpp"
#include "tray.hpp"
#include "tray_icon_png.h"
#include "util.hpp"
#include "util/dialogs.hpp"
#include "util/file_dialogs.hpp"
#include "util/paths.hpp"
#include "util/result.hpp"
#include "util/window.hpp"

using namespace Tray;
using namespace std::chrono_literals;

static std::mutex              mtx;
static std::condition_variable cv;
static std::atomic<bool>       quit{ false };
#if !OSHOT_TOOL_ON_MAIN_THREAD
// The next capture for capture_worker()
static bool                      do_capture = false;
static std::string               pending_source;   // image to open instead of capturing, empty for a screenshot
static std::chrono::milliseconds pending_delay{};  // wait this long before capturing
#endif
static std::atomic<bool> capture_active{ false };  // a run_main_tool() is in progress, on any thread
static bool              owns_tray_lock = false;   // false in the extra trays allow-multiple-trays lets start

bool app_quitting()
{
    return quit.load();
}

// Has to stay async-signal-safe, so it only raises the flag and lets the
// capture and tray loops act on it. Once the tray is up, Tray::SetQuitSignalCallback() takes over.
void on_quit_signal(int)
{
    quit.store(true);
}

// Quit from the tray menu, a signal or the system (tray or main thread).
// A capture in progress sees app_quitting() and closes itself.
void request_quit()
{
    {
        std::scoped_lock lk(mtx);
        quit.store(true);
    }
    cv.notify_all();
    trayMaker.Exit();
}

void exit_handler_nc()
{
    static std::atomic_flag ran = ATOMIC_FLAG_INIT;
    if (ran.test_and_set())
        return;

    request_quit();

    // Quitting SDL under a running capture (e.g. dialog::die() from the worker
    // thread) would crash it instead of exiting
    if (!capture_active.load())
        SDL_Quit();
}

static int run_overlay()
{
    std::optional<ProcessLock> lock;
    if (!g_config->File.allow_multi_overlay)
    {
        // If the lock can't be created, an unguarded overlay beats none at all
        lock.emplace("oshot-overlay");
        if (lock->busy())
        {
            spdlog::info("Another overlay is already open, ignoring this capture");
            return EXIT_SUCCESS;
        }
    }

    return run_main_tool();
}

// Called after every capture. The tray process sits waiting for the next one and would
// otherwise keep the last screenshot and the OCR model until then
static void release_capture_memory()
{
    g_ss_tool.EndCapture();
#ifdef __GLIBC__
    // glibc keeps freed memory for reuse, mostly in the capture_worker thread's
    // arena where the overlay allocated it, so hand the free pages in there back.
    // The arena's free top only shrinks with the fixed threshold set in oshot_main()
    malloc_trim(0);
#endif
}

int run_capture()
{
    capture_active.store(true);
    const int ret = run_overlay();
    release_capture_memory();
    capture_active.store(false);
    return ret;
}

#if !OSHOT_TOOL_ON_MAIN_THREAD  // Linux, where captures run on this worker
// The tray keeps SDL, and so the graphics driver, loaded between captures, which opens the
// next overlay ~50 ms sooner. The driver also keeps its buffers (~25 MB of GPU memory
// at 1080p) and 10-30 MB of RAM until SDL quits, so do that after this long idle.
// On X11 SDL never unloads libGL (SDL_x11opengl.c), so there the driver's code stays loaded
static constexpr std::chrono::seconds GL_IDLE_RELEASE{ 30 };

static void capture_worker()
{
    bool gl_loaded = false;
    while (!quit.load())
    {
        // wait for command
        std::unique_lock lk(mtx);
        const auto       has_command = [] { return quit.load() || do_capture; };
        if (!gl_loaded)
        {
            cv.wait(lk, has_command);
        }
        else if (!cv.wait_for(lk, GL_IDLE_RELEASE, has_command))
        {
            lk.unlock();
            // Not under an open file dialog (the tray's Open Image), see file_dialog_mutex().
            // Try again after another idle period
            const std::unique_lock dialogs(dialog::file_dialog_mutex(), std::try_to_lock);
            if (!dialogs.owns_lock())
                continue;
            // On this thread, where the captures called SDL_Init()
            SDL_Quit();
            gl_loaded = false;
#  ifdef __GLIBC__
            malloc_trim(0);
#  endif
            continue;
        }
        if (quit.load())
            break;

        do_capture                    = false;
        g_config->Runtime.source_file = std::move(pending_source);
        pending_source.clear();
        const auto delay = std::exchange(pending_delay, {});

        // Set under the lock so capture_from_tray() can't queue another one in between
        capture_active.store(true);
        lk.unlock();
        std::this_thread::sleep_for(delay);
        run_overlay();
        release_capture_memory();
        gl_loaded = true;

        lk.lock();
        capture_active.store(false);
        lk.unlock();
        cv.notify_all();
    }
}
#endif

// The tray menu takes a moment to fade out after the click, and a capture started
// right away would have the menu in the screenshot
static constexpr std::chrono::milliseconds TRAY_MENU_FADE{ 300 };

// What in the tray started a capture
enum class TrayClick : std::uint8_t
{
    None,  // hotkey, reopen, Open Image
    Icon,  // primary click on the icon
    Menu   // the Capture menu item
};

static void capture_from_tray(const std::string& source = {}, TrayClick click = TrayClick::None);

// (Re-)register the global hotkey from the config if it changed
static void sync_hotkey()
{
#if OSHOT_HAS_GLOBAL_HOTKEY
    // The first tray has it registered already, so another tray would only get an error
    if (!owns_tray_lock)
        return;

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
        // status() is translated, but logs stay in English
        spdlog::info("Global hotkey: {}", hk.get().empty() ? "disabled" : hotkey::to_string(hk.get()));
        return;
    }

    spdlog::warn("Global hotkey: {}", res.error_v());
    // status() has the same error in the user's language; a config the parser rejects only has the English one
    const std::string& body = i18n::tr_fmt("{}\nPick another shortcut in Preferences, or set 'hotkey' in the config.",
                                           hk.ok() ? hotkey::status() : res.error_v());
    NvdNotification*   notif =
        dialog::init() ? nvd_notification_new(_("oshot: hotkey not set"), body.c_str(), NVD_NOTIFICATION_WARNING)
                       : nullptr;
    if (notif)
    {
        nvd_send_notification(notif);
        nvd_delete_notification(notif);
    }
#endif
}

// Open another overlay on top of this one in a separate `oshot --gui` process, since a process has a single
// overlay (one SDL window, ImGui context and g_ss_tool). Its screenshot has the current overlay in it
static void spawn_gui_instance(const std::string& source)
{
    // It would only find the overlay lock taken and exit
    if (!g_config->File.allow_multi_overlay)
    {
        spdlog::debug("A capture is already open, and allow-multiple-overlays is off");
        return;
    }

    // Holding or mashing the hotkey shouldn't fork-bomb overlays
    static std::chrono::steady_clock::time_point last_spawn;

    const auto now = std::chrono::steady_clock::now();
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
    // We got the hotkey/tray click, so we're allowed to take the foreground. Pass that
    // on, otherwise the new overlay can open behind the current one
    AllowSetForegroundWindow(ASFW_ANY);
#endif

    // Waited on from its own thread so it doesn't stay a zombie on Unix
    auto proc = std::make_shared<TinyProcessLib::Process>(args, string_type{});
    std::thread([proc] {
        const int code = proc->get_exit_status();
        if (code != 0)
            spdlog::warn("Overlay process exited with code {}", code);
    }).detach();
}

// Start a capture from the tray (menu or global hotkey) of a fresh screenshot or the `source` image, after the
// menu fades out with `click` set. The source is always set, or an image opened once would come back in every
// later capture
static void capture_from_tray(const std::string& source, const TrayClick click)
{
    if (click == TrayClick::Icon)
    {
        // A double click shouldn't open a second overlay on top of the first
        static std::chrono::steady_clock::time_point last_click;

        const auto now = std::chrono::steady_clock::now();
        if (now - last_click < 500ms)
            return;
        last_click = now;
    }

#if OSHOT_TOOL_ON_MAIN_THREAD
    if (app_quitting())
        return;

    bool wait_fade = click == TrayClick::Menu;
#  if OSHOT_WINDOWS
    // The overflow flyout, if the icon is in it, stays open over the capture.
    // Send it an Esc like the user would. Hiding it directly leaves Explorer
    // thinking it's open, and then the next click on its arrow does nothing
    HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
    if (click != TrayClick::None && overflow && IsWindowVisible(overflow))
    {
        PostMessageW(overflow, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
        PostMessageW(overflow, WM_KEYUP, VK_ESCAPE, 0xC0010001);
        wait_fade = true;
    }
#  endif

    if (wait_fade)
        std::this_thread::sleep_for(TRAY_MENU_FADE);

    // The SDL event pump inside run_main_tool() also dispatches the tray's
    // messages, so a second capture can't run in here (it would end up nested
    // inside the first one). Stack another overlay process on top instead.
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
    // SDL_Init() finishing the app launch replaces the reopen handler
    Tray::SetReopenCallback([] { capture_from_tray(); });
#  endif
#else
    {
        std::scoped_lock lk(mtx);
        // Already queued, about to start
        if (do_capture || quit.load())
            return;

        if (!capture_active.load())
        {
            pending_source = source;
            pending_delay  = click == TrayClick::Menu ? TRAY_MENU_FADE : std::chrono::milliseconds{};
            do_capture     = true;
            cv.notify_all();
            return;
        }
    }
    spawn_gui_instance(source);
#endif
}

// Run as the tray, or capture once when another process owns it
int run_tray()
{
    std::error_code ec;

    const ProcessLock tray_lock("oshot_tray");
    owns_tray_lock = tray_lock.held();

    if (g_config->Runtime.only_launch_tray)
    {
        if (!owns_tray_lock && !g_config->File.allow_multi_tray)
        {
            spdlog::info("oshot is already running in the system tray");
            return EXIT_SUCCESS;
        }

        g_is_systray = true;
    }
    else if (!owns_tray_lock)
    {
        // Another instance owns the tray, just capture
        return run_capture();
    }

#ifdef __GLIBC__
    // This process stays up as the tray. After a big block is freed (a screenshot,
    // the OCR model) glibc raises its thresholds to that size and then keeps up to
    // twice as much free memory around. A fixed threshold gives it back after each
    // capture (~50 MB). Costs ~10 ms on an OCR of a whole 1080p screen
    mallopt(M_MMAP_THRESHOLD, 1024 * 1024);
#endif

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

    // Set up dialogs here on the main thread, where the tray runs.
    // The worker only reaches dialog::init() once its overlay is on
    // screen, so this call gets there first and the worker's is a no-op.
    dialog::init();

    // The white icon on a dark panel. Anywhere else, or when that can't be told,
    // the full-color one, since it shows on any panel
#if OSHOT_WINDOWS
    const std::string white_icon = (get_exe_dir() / "oshot-tray.ico").string();
    const std::string color_icon = get_exe_path().string();
    TrayIcon          tray       = { "oshot.png", color_icon, "oshot", {} };

    const auto pick_tray_icon = [&] {
        tray.iconFileIco = Tray::PanelIsDark() && fs::exists(white_icon, ec) ? white_icon : color_icon;
    };
#else
    const auto write_png = [](const fs::path& path, const unsigned char* data, const unsigned int len) {
        std::ofstream out(path.string(), std::ios::binary | std::ios::out | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), std::streamsize(len));
    };

    const fs::path white_icon = get_cache_dir() / "tray-icon-white.png";
    const fs::path color_icon = get_cache_dir() / "tray-icon.png";
    fs::create_directories(white_icon.parent_path(), ec);
    write_png(white_icon, tray_icon_png, tray_icon_png_len);
    write_png(color_icon, oshot_png, oshot_png_len);
    TrayIcon tray = { .iconFilePng = color_icon.string(), .iconFileIco = "oshot.ico", .tooltip = "oshot", .menu = {} };

    // macOS draws the white one as a template image, dark or light to match the menu bar
    const auto pick_tray_icon = [&] {
        tray.iconFilePng = (OSHOT_MACOS || Tray::PanelIsDark() ? white_icon : color_icon).string();
    };
#endif
    pick_tray_icon();

    TrayMenu capture_item{ .text        = _("Capture"),
                           .isEnabled   = true,
                           .isChecked   = false,
                           .hasCheckbox = false,
                           .onClicked   = [&](TrayMenu*) { capture_from_tray({}, TrayClick::Menu); },
                           .subMenu = {} };

    TrayMenu open_item{ .text        = _("Open Image"),
                        .isEnabled   = true,
                        .isChecked   = false,
                        .hasCheckbox = false,
                        .onClicked =
                            [&](TrayMenu*) {
                                // Patterns are case-sensitive with zenity
                                constexpr SDL_DialogFileFilter filter{ .name    = N_("Images"),
                                                                       .pattern = "png;jpeg;jpg;bmp;PNG;JPEG;JPG;BMP" };

                                const std::optional<std::string>& open_path =
                                    dialog::open_file(_("Open Image"), "", filter);

                                // Canceled the dialog
                                if (!open_path)
                                    return;

                                capture_from_tray(*open_path);
                            },
                        .subMenu = {} };

    TrayMenu quit_item{ .text        = _("Quit"),
                        .isEnabled   = true,
                        .isChecked   = false,
                        .hasCheckbox = false,
                        .onClicked   = [&](TrayMenu*) { request_quit(); },
                        .subMenu = {} };

    // Locals are fine here, since the tray only uses them while its loop below runs
    tray.menu      = { &capture_item, &open_item, &quit_item };
    tray.onClicked = [] { capture_from_tray({}, TrayClick::Icon); };

    if (trayMaker.Initialize(&tray))
    {
        Tray::SetQuitSignalCallback(request_quit);
        Tray::SetThemeChangeCallback([&] {
            pick_tray_icon();
            trayMaker.Update();
        });

        sync_hotkey();

#if OSHOT_MACOS
        Tray::SetReopenCallback([] { capture_from_tray(); });
        install_quit_handler(request_quit);
#endif

        while (!app_quitting() && trayMaker.Loop(true))
            ;
    }
    else if (g_config->Runtime.only_launch_tray)
    {
        dialog::die("Systray initialization failed");
    }
    else
    {
        // e.g. there's no StatusNotifier host. Still finish the capture the user asked for
        dialog::warn("Systray initialization failed, exiting after the current capture");
#if !OSHOT_TOOL_ON_MAIN_THREAD
        std::unique_lock lk(mtx);
        cv.wait(lk, [] { return quit.load() || (!do_capture && !capture_active.load()); });
#endif
    }

    // The tray loop has exited
    request_quit();
#if !OSHOT_TOOL_ON_MAIN_THREAD
    worker.join();
#endif
    // Not in exit_handler_nc(), since SDL must quit on the main thread with no capture running
    SDL_Quit();

    return EXIT_SUCCESS;
}
