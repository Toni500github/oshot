#include "translation.hpp"

#include <filesystem>
#include <optional>


#include "util.hpp"
#include "fmt/format.h"
#include "tiny-process-library/process.hpp"

void Translator::Start()
{
    if (!HasCurl())
        return;

    m_capabilities |= HAS_CURL;
    if (HasTranslateShell())
        m_capabilities |= HAS_TRANSLATE_BASH;
    if (HasGawk())
        m_capabilities |= HAS_TRANSLATE_GAWK;
}

bool Translator::Has(Capabilities cap)
{
    return m_capabilities & cap;
}

command_result_t Translator::executeCommand(const std::string& command)
{
    command_result_t result;
    std::string      output;

    try
    {
        TinyProcessLib::Process process(
            command,
            "",
            [&output](const char* bytes, size_t n) { output.assign(bytes, n); },
            nullptr,
            true  // enable read from stdout
        );

        result.exit_code = process.get_exit_status();
        result.success   = (result.exit_code == 0);
        result.output    = output;
    }
    catch (...)
    {
        result.success   = false;
        result.output    = "Command execution failed";
        result.exit_code = -1;
    }

    return result;
}

std::optional<std::string> Translator::Translate(const std::string& lang_from, const std::string& lang_to, const std::string& text)
{
    if (m_capabilities == NONE)
        return {};

    if (Has(Capabilities::HAS_TRANSLATE_BASH))
    {
        const command_result_t& result = executeCommand(fmt::format("trans -brief {}:{} $'{}'", lang_from == "auto" ? "" : lang_from, lang_to, replace_str(text, "'", "\\'")));
        if (!result.success)
            return {};
        return result.output;
    }

    return {};
}

bool Translator::HasTranslateShell()
{
#ifndef _WIN32
    // Windows: Check multiple possible locations
    if (executeCommand(WHICH " trans").success)
        return true;
#else
    // Linux/macOS
    if (executeCommand("trans --version 2>&1").output.find("Translate Shell") != (size_t)-1)
        return true;
#endif
    return false;
}

bool Translator::HasCurl()
{
    return executeCommand(WHICH " curl").success;
}

bool Translator::HasGawk()
{
#ifdef _WIN32
    if (std::filesystem::exists(".\\gawk.exe"))
        return true;
#endif
    return (executeCommand(WHICH " gawk").success);
}
