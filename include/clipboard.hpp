#ifndef _CLIPBOARD_HPP_
#define _CLIPBOARD_HPP_

#include <memory>
#include <string>

#include "screen_capture.hpp"

class Clipboard
{
public:
    Clipboard(SessionType session) : m_session(session) {}
    bool CopyText(const std::string& text);
    bool CopyImage(const capture_result_t& cap);
    void SetSession(SessionType session) { m_session = session; }

private:
    SessionType m_session;
};

extern std::unique_ptr<Clipboard> g_clipboard;

#endif  // !_CLIPBOARD_HPP_
