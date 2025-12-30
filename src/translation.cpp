#include "translation.hpp"

#include <filesystem>
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

std::optional<std::string> Translator::Translate(const std::string& lang_from,
                                                 const std::string& lang_to,
                                                 const std::string& text)
{
    const command_result_t& result = config->use_trans_gawk
                                         ? executeCommand(fmt::format("{} -f {} -- -brief {}:{} $'{}'",
                                                                      config->gawk_path,
                                                                      config->trans_awk_path,
                                                                      lang_from == "auto" ? "" : lang_from,
                                                                      lang_to,
                                                                      replace_str(text, "'", "\\'")))
                                         : executeCommand(fmt::format("{} -brief {}:{} $'{}'",
                                                                      config->trans_path,
                                                                      lang_from == "auto" ? "" : lang_from,
                                                                      lang_to,
                                                                      replace_str(text, "'", "\\'")));
    if (!result.success)
        return {};
    return result.output;

    return {};
}

bool Translator::HasCurl()
{
    return executeCommand(WHICH " curl").success;
}
