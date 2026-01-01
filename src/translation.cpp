#include "translation.hpp"

#include <optional>
#include <string>

#include "fmt/format.h"
#include "httplib.h"

std::optional<std::string> Translator::Translate(const std::string& lang_from,
                                                 const std::string& lang_to,
                                                 const std::string& text)
{
    static httplib::Client cli("translate.googleapis.com");
    const std::string&     path = fmt::format("/translate_a/single?client=gtx&sl={}&tl={}&dt=t&q={}",
                                          lang_from,
                                          lang_to,
                                          httplib::encode_uri_component(text));

    if (auto res = cli.Get(path.c_str()))
        if (res->status == 200)
            return parseGoogleResponse(res->body);

    return {};
}

std::string Translator::parseGoogleResponse(const std::string& json)
{
    // Parse JSON: [[["translated text","original",null]],null,"en"]
    std::regex  pattern(R"(\[\"([^\"]+)\")");
    std::smatch match;
    if (std::regex_search(json, match, pattern))
        return match[1];
    return "";
}
