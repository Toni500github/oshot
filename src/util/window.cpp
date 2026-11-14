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

#include "util/window.hpp"

#include "platform.hpp"

#if OSHOT_MACOS
// AssertMacros.h (pulled in by CoreServices) defines check()/verify()/require() otherwise
#  define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#  include <CoreServices/CoreServices.h>
#endif

// Defined and registered in src/main_tool_* source files
namespace main_tool
{
void (*minimize_fn)()       = nullptr;
bool (*hide_fn)()           = nullptr;
void (*show_fn)()           = nullptr;
void* (*dialog_parent_fn)() = nullptr;
}  // namespace main_tool

void register_window_callbacks(void (*minimize_fn)(), bool (*hide_fn)(), void (*show_fn)(), void* (*dialog_parent_fn)())
{
    main_tool::minimize_fn      = minimize_fn;
    main_tool::hide_fn          = hide_fn;
    main_tool::show_fn          = show_fn;
    main_tool::dialog_parent_fn = dialog_parent_fn;
}

void minimize_window()
{
    if (main_tool::minimize_fn)
        main_tool::minimize_fn();
}
bool hide_window()
{
    return main_tool::hide_fn && main_tool::hide_fn();
}
void show_window()
{
    if (main_tool::show_fn)
        main_tool::show_fn();
}
void* dialog_parent_window()
{
    return main_tool::dialog_parent_fn ? main_tool::dialog_parent_fn() : nullptr;
}

#if OSHOT_MACOS
static void (*quit_handler_fn)() = nullptr;

static OSErr on_quit_event(const AppleEvent*, AppleEvent*, SRefCon)
{
    if (quit_handler_fn)
        quit_handler_fn();
    return noErr;
}

void install_quit_handler(void (*on_quit)())
{
    static const AEEventHandlerUPP upp = NewAEEventHandlerUPP(on_quit_event);
    quit_handler_fn                    = on_quit;
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, upp, 0, false);
}
#endif
