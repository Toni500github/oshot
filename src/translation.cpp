#include "translation.hpp"

#include <optional>

#include "config.hpp"
#include "fmt/format.h"
#include "tiny-process-library/process.hpp"
#include "util.hpp"

bool Translator::Start()
{
    return HasCurl();
}

command_result_t Translator::executeCommand(const std::string& command)
{
    command_result_t result;
    std::string      output;

    try
    {
        TinyProcessLib::Process process(
            config->bash_path + " -c \"" + command + "\"",
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

std::optional<std::string> Translator::Translate(const std::string& lang_from,
                                                 const std::string& lang_to,
                                                 const std::string& text)
{
    std::string escaped_str{ text };
    replace_str(escaped_str, "'", "\\'");
    replace_str(escaped_str, "\"", "\\\"");

    const command_result_t& result = executeCommand(fmt::format(
        "{} -brief {}:{} $'{}'", config->trans_path, lang_from == "auto" ? "" : lang_from, lang_to, escaped_str));

    if (!result.success)
        return {};
    return result.output;

    return {};
}

bool Translator::HasCurl()
{
    return executeCommand(WHICH " curl").success;
}
