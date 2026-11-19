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

#ifndef _UTIL_DIAGNOSTICS_HPP_
#define _UTIL_DIAGNOSTICS_HPP_

// What bug reports need: the system oshot runs on, and what happened when it crashed

#include <string>

// ------------------------------
// Macros
// ------------------------------

/* Option for `oshot --crash-report <reason> <log file>`, the process that shows the crash dialog */
#define CRASH_REPORT_ARG "--crash-report"

/* Where crash and error dialogs send users */
#define ISSUES_URL "https://github.com/Toni500github/oshot/issues"

// ------------------------------
// Functions
// ------------------------------

/**
 * Describe the system for bug reports: build, OS, desktop session, packaging and library versions
 * @return One "Name: value" line each, without a trailing newline
 **/
std::string system_info();

/**
 * Catch crashes (fatal signals, uncaught exceptions, unhandled Windows exceptions), write them with a
 * backtrace to stderr and the log file, then start oshot with CRASH_REPORT_ARG to tell the user.
 * Call it once, from the main thread, after spdlog's default logger is set
 * @param log_path The log file, or empty if there's none
 **/
void install_crash_handler(const std::string& log_path);

#endif  // _UTIL_DIAGNOSTICS_HPP_
