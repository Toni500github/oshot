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

#ifndef _UTIL_HPP_
#define _UTIL_HPP_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "platform.hpp"
#include "spdlog/sinks/ringbuffer_sink.h"
#include "util/paths.hpp"

// ------------------------------
// Types
// ------------------------------

/**
 * Changes the working directory, and changes it back when it goes out of scope.
 * Dies if the directory doesn't exist.
 **/
struct CdGuard
{
    fs::path saved;

    CdGuard(const fs::path& p);
    ~CdGuard() { fs::current_path(saved); }
};

/**
 * A named lock shared by this user's oshot processes, taken without waiting and
 * held until it's destroyed or the process exits. A second ProcessLock with the
 * same name in this process is Busy too, just like one in another process.
 **/
class ProcessLock
{
public:
    enum class State : std::uint8_t
    {
        Held,    // we hold it
        Busy,    // another process or ProcessLock holds it
        Failed,  // it couldn't be created (a warning was logged), so nobody can tell
    };

    /**
     * Try to take the lock
     * @param name ASCII name of the lock. It's <runtime dir>/<name>.lock on unix, and the Local\<name> mutex on Windows
     **/
    explicit ProcessLock(std::string_view name);

    ~ProcessLock();

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool  held() const { return m_state == State::Held; }
    [[nodiscard]] bool  busy() const { return m_state == State::Busy; }

    ProcessLock(const ProcessLock&)            = delete;
    ProcessLock& operator=(const ProcessLock&) = delete;

private:
    std::string m_name;
    State       m_state = State::Failed;

#if OSHOT_WINDOWS
    void* m_handle = nullptr;
#else
    int m_fd = -1;
#endif
};

// ------------------------------
// Globals
// ------------------------------

// This process owns the systray
extern bool g_is_systray;

// Video mode size of the monitor the overlay is on
extern int g_scr_w, g_scr_h;

// `/nix` exists, so the OCR window suggests getting tesseract's models from nixpkgs
extern bool g_is_nix;

// Running inside a Flatpak sandbox
extern bool g_is_flatpak;

// The last log lines, shown in the Logs window
extern std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_imgui_log_sink;

// The output of --version, also shown in the About window.
// In globals.cpp, the only file that includes version.h, which changes with every commit
extern const std::string version_infos;

// ------------------------------
// Template functions
// ------------------------------

/**
 * Convert an enum class value to its index, for arrays and bitsets sized by the enum's COUNT
 * @param e The enum value
 **/
template <typename E>
constexpr size_t idx(E e) noexcept
{
    static_assert(std::is_enum_v<E>);
    return static_cast<size_t>(e);
}

/**
 * Convert an index back to an enum class value. The opposite of idx()
 * @param n The index
 **/
template <typename E, typename T>
constexpr E toe(T n) noexcept
{
    static_assert(std::is_integral_v<T>);
    return static_cast<E>(n);
}

#endif  // !_UTIL_HPP_
