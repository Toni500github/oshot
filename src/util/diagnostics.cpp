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

#include "util/diagnostics.hpp"

#include <SDL3/SDL.h>
#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "dotenv.hpp"
#include "fmt/format.h"
#include "platform.hpp"
#include "screen_capture.hpp"
#include "spdlog/spdlog.h"
#include "util.hpp"
#include "util/paths.hpp"

#if OSHOT_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include "fmt/xchar.h"
#else
#  include <fcntl.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define HAVE_EXECINFO 1
#  endif
#endif

#if OSHOT_MACOS
#  include <sys/sysctl.h>
#endif

// ------------------------------
// System info
// ------------------------------

std::string system_info()
{
#ifdef DEBUG
    constexpr const char* build = "Debug";
#else
    constexpr const char* build = "Release";
#endif

    std::string os;
#if OSHOT_WINDOWS
    // GetVersionEx() reports Windows 8 to programs whose manifest doesn't list newer ones
    using rtl_get_version_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW ver{};
    ver.dwOSVersionInfoSize = sizeof(ver);
    const HMODULE ntdll     = GetModuleHandleW(L"ntdll.dll");
    const auto    get_ver =
        ntdll ? reinterpret_cast<rtl_get_version_t>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
              : nullptr;
    if (get_ver && get_ver(&ver) == 0)
        os = fmt::format("Windows {}.{}.{}", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber);
    else
        os = "Windows";
#  if defined(__aarch64__) || defined(_M_ARM64)
    os += " arm64";
#  else
    os += " x64";
#  endif
#else
    struct utsname uts{};
    if (uname(&uts) == 0)
        os = fmt::format("{} {} {}", uts.sysname, uts.release, uts.machine);
#  if OSHOT_MACOS
    char   product[32]{};
    size_t size = sizeof(product);
    if (sysctlbyname("kern.osproductversion", product, &size, nullptr, 0) == 0)
        os = fmt::format("macOS {} ({})", product, os);
#  else
    // A Flatpak sees its runtime's os-release, and the host's under /run/host
    Dotenv os_release;
    if (os_release.load(g_is_flatpak ? "/run/host/os-release" : "/etc/os-release"))
        os = fmt::format("{} ({})", os_release.get("PRETTY_NAME", "<unknown>"), os);
#  endif
#endif

    const char* session = "<unknown>";
    switch (get_session_type())
    {
        case SessionType::Wayland: session = "Wayland"; break;
        case SessionType::X11:     session = "X11"; break;
        case SessionType::Windows: session = "Windows"; break;
        case SessionType::MacOS:   session = "macOS"; break;
        case SessionType::KDE:     session = "KDE"; break;
        case SessionType::Unknown: break;
    }

    const int   sdl  = SDL_GetVersion();
    std::string info = fmt::format(
        "Build: {}\n"
        "OS: {}\n"
        "Capture backend: {}\n",
        build,
        os,
        session);

#if OSHOT_FREEDESKTOP
    const auto env = [](const char* name) {
        const char* value = std::getenv(name);
        return (value && *value) ? value : "<unset>";
    };
    info += fmt::format("Session: XDG_SESSION_TYPE={} XDG_CURRENT_DESKTOP={} WAYLAND_DISPLAY={} DISPLAY={}\n",
                        env("XDG_SESSION_TYPE"),
                        env("XDG_CURRENT_DESKTOP"),
                        env("WAYLAND_DISPLAY"),
                        env("DISPLAY"));
    if (g_is_flatpak)
        info += "Package: Flatpak\n";
    else if (std::getenv("APPIMAGE"))
        info += "Package: AppImage\n";
#endif

    info += fmt::format("Libraries: SDL {}.{}.{}, Tesseract {}, Leptonica {}.{}.{}, ZBar {}",
                        SDL_VERSIONNUM_MAJOR(sdl),
                        SDL_VERSIONNUM_MINOR(sdl),
                        SDL_VERSIONNUM_MICRO(sdl),
                        tesseract::TessBaseAPI::Version(),
                        LIBLEPT_MAJOR_VERSION,
                        LIBLEPT_MINOR_VERSION,
                        LIBLEPT_PATCH_VERSION,
                        ZBAR_PKG_VERSION);
    return info;
}

// ------------------------------
// Crash handler
// ------------------------------

// Set up front, since the handlers can't safely allocate
static std::atomic_flag crashing = ATOMIC_FLAG_INIT;
static std::string      crash_exe;
static std::string      crash_log;

#if OSHOT_WINDOWS
static void report_crash(const std::string& reason, void* fault_address)
{
    // MinGW builds have no PDBs for DbgHelp, so frames are module offsets for addr2line
    const auto describe = [](void* addr) {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH]{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(addr),
                                &module) ||
            GetModuleFileNameW(module, path, MAX_PATH) == 0)
            return fmt::format("{}", addr);
        return fmt::format(
            "{}+{:#x}", fs::path(path).filename().string(), uintptr_t(addr) - reinterpret_cast<uintptr_t>(module));
    };

    std::string text = fmt::format("\n*** oshot crashed with {} ***\n", reason);
    if (fault_address)
        text += fmt::format("At {}\n", describe(fault_address));
    void*        frames[62];
    const USHORT count = CaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < count; ++i)
        text += fmt::format("#{} {}\n", i, describe(frames[i]));

    std::fputs(text.c_str(), stderr);
    if (FILE* log = crash_log.empty() ? nullptr : std::fopen(crash_log.c_str(), "ab"))
    {
        std::fputs(text.c_str(), log);
        std::fclose(log);
    }

    if (crash_exe.empty())
        return;
    std::wstring cmd = fmt::format(L"\"{}\" " CRASH_REPORT_ARG " \"{}\" \"{}\"",
                                   fs::path(crash_exe).wstring(),
                                   fs::path(reason).wstring(),
                                   fs::path(crash_log).wstring());

    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static LPTOP_LEVEL_EXCEPTION_FILTER mingw_filter = nullptr;

static LONG WINAPI on_unhandled_exception(EXCEPTION_POINTERS* info)
{
    // An uncaught C++ exception ("GCC" in the code) comes here before std::terminate(), and MinGW's own
    // filter lets it continue so the terminate handler runs
    if ((info->ExceptionRecord->ExceptionCode & 0x00FFFFFF) == 0x474343)
        return mingw_filter ? mingw_filter(info) : EXCEPTION_CONTINUE_EXECUTION;

    // The first crashing thread reports, the others wait for it to end the process
    if (crashing.test_and_set())
        Sleep(INFINITE);
    report_crash(fmt::format("exception {:#010x}", info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void on_abort(int)
{
    if (crashing.test_and_set())
        Sleep(INFINITE);
    report_crash("SIGABRT", nullptr);
}
#else
static int crash_fd = -1;

// Only async-signal-safe calls from here on
static void write_crash(const char* str)
{
    const size_t len = std::strlen(str);
    // Nothing to do if a write fails, but glibc warns about ignoring the result
    [[maybe_unused]] ssize_t written = 0;
    if (crash_fd != -1)
        written = write(crash_fd, str, len);
    written = write(STDERR_FILENO, str, len);
}

static void on_fatal_signal(int sig)
{
    // The first crashing thread reports, the others wait for it to end the process
    if (crashing.test_and_set())
        for (;;)
            pause();

    const char* name = sig == SIGSEGV  ? "SIGSEGV"
                       : sig == SIGBUS ? "SIGBUS"
                       : sig == SIGFPE ? "SIGFPE"
                       : sig == SIGILL ? "SIGILL"
                                       : "SIGABRT";
    write_crash("\n*** oshot crashed with ");
    write_crash(name);
    write_crash(" ***\n");
#  ifdef HAVE_EXECINFO
    void*     frames[64];
    const int count = backtrace(frames, 64);
    if (crash_fd != -1)
        backtrace_symbols_fd(frames, count, crash_fd);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#  endif

    // Not fork(), whose atfork handlers may wait for a lock the crash left held, nor posix_spawn(), which
    // isn't async-signal-safe
    const char* argv[] = { crash_exe.c_str(), CRASH_REPORT_ARG, name, crash_log.c_str(), nullptr };
    if (!crash_exe.empty() && vfork() == 0)  // NOLINT(bugprone-unsafe-functions)
    {
        execv(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }

    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void install_crash_handler(const std::string& log_path)
{
    crash_log = log_path;
    crash_exe = get_exe_path().string();
#if OSHOT_LINUX
    // Like spawn_gui_instance(), since the inner binary can't find the bundled libs without AppRun
    if (const char* appimage = std::getenv("APPIMAGE"))
        crash_exe = appimage;
#endif

    std::set_terminate([] {
        try
        {
            if (const std::exception_ptr e = std::current_exception())
                std::rethrow_exception(e);
            spdlog::critical("std::terminate() called without an exception");
        }
        catch (const std::exception& e)
        {
            spdlog::critical("Uncaught exception: {}", e.what());
        }
        catch (...)
        {
            spdlog::critical("Uncaught exception of an unknown type");
        }
        std::abort();
    });

#if OSHOT_WINDOWS
    // Room for the handler after a stack overflow
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    mingw_filter = SetUnhandledExceptionFilter(on_unhandled_exception);
    signal(SIGABRT, on_abort);
    // No "abort() has been called" box or Windows Error Reporting, the crash dialog replaces them
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
    if (!log_path.empty())
        crash_fd = open(log_path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
#  ifdef HAVE_EXECINFO
    // The first call loads libgcc, which can't happen in a signal handler
    void* frame = nullptr;
    backtrace(&frame, 1);
#  endif

    // A stack of its own, so a stack overflow on the main thread gets reported too
    static char alt_stack[64 * 1024];
    stack_t     stack{};
    stack.ss_sp   = alt_stack;
    stack.ss_size = sizeof(alt_stack);
    sigaltstack(&stack, nullptr);

    struct sigaction action{};
    action.sa_handler = on_fatal_signal;
    action.sa_flags   = SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT })
        sigaction(sig, &action, nullptr);
#endif
}
