#include "translation.hpp"

#include <optional>
#include <string>

#include "httplib.h"

std::optional<std::string> Translator::Translate(const std::string& lang_from,
                                                 const std::string& lang_to,
                                                 const std::string& text)
{
    static httplib::Client  cli("translate.googleapis.com");
    static httplib::Headers headers = { { "Content-Type", "application/x-www-form-urlencoded" },
                                        { "User-Agent", "Mozilla/5.0" } };

    // https://github.com/matheuss/google-translate-api/blob/777d7db94f82ec402e7758af1549818c07d55747/index.js#L32
    httplib::Params params = {
        { "sl", lang_from }, { "tl", lang_to }, { "hl", lang_to }, { "client", "gtx" },
        { "ie", "UTF-8" },   { "oe", "UTF-8" }, { "dt", "t" },     { "dt", "bd" },
        { "dt", "rw" },      { "dt", "rm" },    { "dt", "ss" },    { "dt", "qca" },
        { "dt", "ld" },      { "dt", "at" },    { "dt", "gt" },    { "otf", "1" },
        { "ssel", "0" },     { "tsel", "0" },   { "kc", "7" },     { "q", httplib::encode_uri_component(text) },
    };

    if (auto res = cli.Post("/translate_a/single", headers, params))
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
        return httplib::decode_uri_component(match[1]);
    return "";
}
