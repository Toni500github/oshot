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

#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <vector>

#include "fmt/format.h"
#include "platform.hpp"
#include "util/dialogs.hpp"
#include "util/paths.hpp"

#if OSHOT_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

CdGuard::CdGuard(const fs::path& p) : saved(fs::current_path())
{
    if (!fs::exists(p))
        dialog::die("CdGuard: Path {} doesn't exist", p.string());
    if (!p.empty())
        fs::current_path(p);
}

// Names of the ProcessLocks this process holds. fcntl() locks never conflict within a
// process, and closing any fd of the file would drop ours, so a second one has to be refused here
static std::mutex               g_held_locks_mtx;
static std::vector<std::string> g_held_locks;

ProcessLock::ProcessLock(const std::string_view name) : m_name(name)
{
    std::scoped_lock lk(g_held_locks_mtx);
    if (std::ranges::find(g_held_locks, m_name) != g_held_locks.end())
    {
        m_state = State::Busy;
        return;
    }

#if OSHOT_WINDOWS
    // "Local\\" is per user session, while "Global\\" is system-wide and sometimes needs privileges
    const std::wstring wide  = L"Local\\" + std::wstring(m_name.begin(), m_name.end());
    HANDLE             mutex = CreateMutexW(nullptr, FALSE, wide.c_str());
    if (!mutex)
    {
        spdlog::warn("Failed to create the {} lock: error {}", m_name, GetLastError());
        return;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        m_state = State::Busy;
        return;
    }

    m_handle = mutex;
#else
    // Without XDG_RUNTIME_DIR this is the shared /tmp, where another user's
    // 0600 lock file would make open() fail and look like "already running"
    const char*       xdg       = ::getenv("XDG_RUNTIME_DIR");
    const std::string lock_name = xdg ? fmt::format("{}.lock", m_name) : fmt::format("{}-{}.lock", m_name, getuid());
    const std::string lock_path = (get_runtime_dir() / lock_name).string();

    // O_CLOEXEC: don't leak the fd into grim/wl-copy/zenity children
    const int fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        spdlog::warn("Failed to open the lock file {}: {}", lock_path, strerror(errno));
        return;
    }

    struct flock fl{};
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;

    if (fcntl(fd, F_SETLK, &fl) == -1)
    {
        close(fd);
        m_state = State::Busy;
        return;
    }

    m_fd = fd;
#endif

    m_state = State::Held;
    g_held_locks.push_back(m_name);
}

ProcessLock::~ProcessLock()
{
    if (m_state != State::Held)
        return;

#if OSHOT_WINDOWS
    CloseHandle(m_handle);
#else
    close(m_fd);
#endif

    std::scoped_lock lk(g_held_locks_mtx);
    std::erase(g_held_locks, m_name);
}
