#ifndef _CLIPBOARD_HPP_
#define _CLIPBOARD_HPP_

#include <string>

#include "screen_capture.hpp"

class Clipboard
{
public:
    Clipboard(SessionType session) : m_session(session) {}
    void     SetSession(SessionType session) { m_session = session; }
    Result<> CopyText(const std::string& text);
    Result<> CopyImage(const capture_result_t& cap);

private:
    SessionType m_session;
};

extern Clipboard g_clipboard;

#endif  // !_CLIPBOARD_HPP_
