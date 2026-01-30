#ifndef _TRANSLATION_HPP_
#define _TRANSLATION_HPP_

#include <string>

#include "util.hpp"

class Translator
{
public:
    Result<std::string> Translate(const std::string& lang_from, const std::string& lang_to, const std::string& text);

private:
    Result<std::string> parseGoogleResponse(const std::string& json);
};

#endif  // !_TRANSLATION_HPP_
