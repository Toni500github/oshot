#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#define WHICH "where"
#else
#include <unistd.h>
#define WHICH "which"
#endif

enum Capabilities
{
    NONE               = 0,
    HAS_CURL           = 1 << 1,
    HAS_TRANSLATE_BASH = 1 << 2,
    HAS_TRANSLATE_GAWK = 1 << 3,
};

struct command_result_t
{
    bool        success;
    std::string output;
    int         exit_code;
};

class Translator
{
public:
    void Start();

    bool Has(Capabilities cap);

    static command_result_t executeCommand(const std::string& command);

    std::optional<std::string> Translate(const std::string& lang_from, const std::string& lang_to, const std::string& text);

private:
    int m_capabilities = NONE;

    bool HasTranslateShell();

    bool HasCurl();

    bool HasGawk();
};
