#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#define WHICH "where"
#else
#include <unistd.h>
#define WHICH "which"
#endif

struct command_result_t
{
    bool        success;
    std::string output;
    int         exit_code;
};

class Translator
{
public:
    bool Start();

    bool HasCurl();

    static command_result_t executeCommand(const std::string& command);

    std::optional<std::string> Translate(const std::string& lang_from,
                                         const std::string& lang_to,
                                         const std::string& text);
};
