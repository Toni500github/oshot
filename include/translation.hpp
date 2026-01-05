#ifndef _TRANSLATION_HPP_
#define _TRANSLATION_HPP_

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

#endif  // !_TRANSLATION_HPP_
