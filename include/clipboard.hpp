#ifndef _CLIPBOARD_HPP_
#define _CLIPBOARD_HPP_

#include <memory>
#include <string>

#include "screen_capture.hpp"

class Clipboard
{
public:
    Clipboard(SessionType session) : m_session(session) {}
    Result<> CopyText(const std::string& text);
    Result<> CopyImage(const capture_result_t& cap);

private:
    SessionType m_session;
};

extern bool                       g_is_clipboard_server;
extern std::unique_ptr<Clipboard> g_clipboard;

#endif  // !_CLIPBOARD_HPP_
