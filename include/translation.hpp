#include <optional>
#include <string>

class Translator
{
public:
    std::optional<std::string> Translate(const std::string& lang_from,
                                         const std::string& lang_to,
                                         const std::string& text);

private:
    std::string parseGoogleResponse(const std::string& json);
};
