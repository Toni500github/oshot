#include <array>
#include <utility>

#include "fmt/base.h"
#include "frozen/string.h"
#include "frozen/unordered_map.h"
#include "switch_fnv1a.hpp"

using namespace frozen::string_literals;

constexpr std::array<std::pair<const char*, const char*>, 114> GOOGLE_TRANSLATE_LANGUAGES_ARRAY = {
    { { "auto", "Automatic" },
      { "af", "Afrikaans" },
      { "sq", "Albanian" },
      { "am", "Amharic" },
      { "ar", "Arabic" },
      { "hy", "Armenian" },
      { "az", "Azerbaijani" },
      { "eu", "Basque" },
      { "be", "Belarusian" },
      { "bn", "Bengali" },
      { "bs", "Bosnian" },
      { "bg", "Bulgarian" },
      { "ca", "Catalan" },
      { "ceb", "Cebuano" },
      { "ny", "Chichewa" },
      { "co", "Corsican" },
      { "hr", "Croatian" },
      { "cs", "Czech" },
      { "da", "Danish" },
      { "nl", "Dutch" },
      { "en", "English" },
      { "eo", "Esperanto" },
      { "et", "Estonian" },
      { "tl", "Filipino" },
      { "fi", "Finnish" },
      { "fr", "French" },
      { "fy", "Frisian" },
      { "gl", "Galician" },
      { "ka", "Georgian" },
      { "de", "German" },
      { "el", "Greek" },
      { "gu", "Gujarati" },
      { "ht", "Haitian Creole" },
      { "ha", "Hausa" },
      { "haw", "Hawaiian" },
      { "he", "Hebrew" },
      { "hi", "Hindi" },
      { "hmn", "Hmong" },
      { "hu", "Hungarian" },
      { "is", "Icelandic" },
      { "ig", "Igbo" },
      { "id", "Indonesian" },
      { "ga", "Irish" },
      { "it", "Italian" },
      { "ja", "Japanese" },
      { "jw", "Javanese" },
      { "kn", "Kannada" },
      { "kk", "Kazakh" },
      { "km", "Khmer" },
      { "ko", "Korean" },
      { "ku", "Kurdish (Kurmanji)" },
      { "ky", "Kyrgyz" },
      { "lo", "Lao" },
      { "la", "Latin" },
      { "lv", "Latvian" },
      { "lt", "Lithuanian" },
      { "lb", "Luxembourgish" },
      { "mk", "Macedonian" },
      { "mg", "Malagasy" },
      { "ms", "Malay" },
      { "ml", "Malayalam" },
      { "mt", "Maltese" },
      { "mi", "Maori" },
      { "mr", "Marathi" },
      { "mn", "Mongolian" },
      { "my", "Myanmar (Burmese)" },
      { "ne", "Nepali" },
      { "no", "Norwegian" },
      { "ps", "Pashto" },
      { "fa", "Persian" },
      { "pl", "Polish" },
      { "pt", "Portuguese" },
      { "pa", "Punjabi" },
      { "ro", "Romanian" },
      { "ru", "Russian" },
      { "sm", "Samoan" },
      { "gd", "Scots Gaelic" },
      { "sr", "Serbian" },
      { "st", "Sesotho" },
      { "sn", "Shona" },
      { "sd", "Sindhi" },
      { "si", "Sinhala" },
      { "sk", "Slovak" },
      { "sl", "Slovenian" },
      { "so", "Somali" },
      { "es", "Spanish" },
      { "su", "Sundanese" },
      { "sw", "Swahili" },
      { "sv", "Swedish" },
      { "tg", "Tajik" },
      { "ta", "Tamil" },
      { "te", "Telugu" },
      { "th", "Thai" },
      { "tr", "Turkish" },
      { "uk", "Ukrainian" },
      { "ur", "Urdu" },
      { "uz", "Uzbek" },
      { "vi", "Vietnamese" },
      { "cy", "Welsh" },
      { "xh", "Xhosa" },
      { "yi", "Yiddish" },
      { "yo", "Yoruba" },
      { "zu", "Zulu" },
      { "zh", "Chinese (Auto)" },
      { "zh-CN", "Chinese (Simplified)" },
      { "zh-TW", "Chinese (Traditional)" },
      { "pt-br", "Portuguese (Brazil)" },
      { "pt-pt", "Portuguese (Portugal)" },
      { "es-419", "Spanish (Latin America)" },
      { "es-es", "Spanish (Spain)" },
      { "en-gb", "English (UK)" },
      { "en-us", "English (US)" },
      { "fr-ca", "French (Canada)" },
      { "fr-fr", "French (France)" } }
};

// Custom formatter for the pair
template <>
struct fmt::formatter<std::pair<const char*, const char*>>
{
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const std::pair<const char*, const char*>& p, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}: {}", p.second, p.first);
    }
};

static constexpr std::string_view getNameFromCode(const std::string_view code)
{
    switch (fnv1a32::hash(code))
    {
        case "auto"_fnv1a32:   return "Automatic";
        case "af"_fnv1a32:     return "Afrikaans";
        case "sq"_fnv1a32:     return "Albanian";
        case "am"_fnv1a32:     return "Amharic";
        case "ar"_fnv1a32:     return "Arabic";
        case "hy"_fnv1a32:     return "Armenian";
        case "az"_fnv1a32:     return "Azerbaijani";
        case "eu"_fnv1a32:     return "Basque";
        case "be"_fnv1a32:     return "Belarusian";
        case "bn"_fnv1a32:     return "Bengali";
        case "bs"_fnv1a32:     return "Bosnian";
        case "bg"_fnv1a32:     return "Bulgarian";
        case "ca"_fnv1a32:     return "Catalan";
        case "ceb"_fnv1a32:    return "Cebuano";
        case "ny"_fnv1a32:     return "Chichewa";
        case "zh-CN"_fnv1a32:  return "Chinese (Simplified)";
        case "zh-TW"_fnv1a32:  return "Chinese (Traditional)";
        case "co"_fnv1a32:     return "Corsican";
        case "hr"_fnv1a32:     return "Croatian";
        case "cs"_fnv1a32:     return "Czech";
        case "da"_fnv1a32:     return "Danish";
        case "nl"_fnv1a32:     return "Dutch";
        case "en"_fnv1a32:     return "English";
        case "eo"_fnv1a32:     return "Esperanto";
        case "et"_fnv1a32:     return "Estonian";
        case "tl"_fnv1a32:     return "Filipino";
        case "fi"_fnv1a32:     return "Finnish";
        case "fr"_fnv1a32:     return "French";
        case "fy"_fnv1a32:     return "Frisian";
        case "gl"_fnv1a32:     return "Galician";
        case "ka"_fnv1a32:     return "Georgian";
        case "de"_fnv1a32:     return "German";
        case "el"_fnv1a32:     return "Greek";
        case "gu"_fnv1a32:     return "Gujarati";
        case "ht"_fnv1a32:     return "Haitian Creole";
        case "ha"_fnv1a32:     return "Hausa";
        case "haw"_fnv1a32:    return "Hawaiian";
        case "he"_fnv1a32:     return "Hebrew";
        case "hi"_fnv1a32:     return "Hindi";
        case "hmn"_fnv1a32:    return "Hmong";
        case "hu"_fnv1a32:     return "Hungarian";
        case "is"_fnv1a32:     return "Icelandic";
        case "ig"_fnv1a32:     return "Igbo";
        case "id"_fnv1a32:     return "Indonesian";
        case "ga"_fnv1a32:     return "Irish";
        case "it"_fnv1a32:     return "Italian";
        case "ja"_fnv1a32:     return "Japanese";
        case "jw"_fnv1a32:     return "Javanese";
        case "kn"_fnv1a32:     return "Kannada";
        case "kk"_fnv1a32:     return "Kazakh";
        case "km"_fnv1a32:     return "Khmer";
        case "ko"_fnv1a32:     return "Korean";
        case "ku"_fnv1a32:     return "Kurdish (Kurmanji)";
        case "ky"_fnv1a32:     return "Kyrgyz";
        case "lo"_fnv1a32:     return "Lao";
        case "la"_fnv1a32:     return "Latin";
        case "lv"_fnv1a32:     return "Latvian";
        case "lt"_fnv1a32:     return "Lithuanian";
        case "lb"_fnv1a32:     return "Luxembourgish";
        case "mk"_fnv1a32:     return "Macedonian";
        case "mg"_fnv1a32:     return "Malagasy";
        case "ms"_fnv1a32:     return "Malay";
        case "ml"_fnv1a32:     return "Malayalam";
        case "mt"_fnv1a32:     return "Maltese";
        case "mi"_fnv1a32:     return "Maori";
        case "mr"_fnv1a32:     return "Marathi";
        case "mn"_fnv1a32:     return "Mongolian";
        case "my"_fnv1a32:     return "Myanmar (Burmese)";
        case "ne"_fnv1a32:     return "Nepali";
        case "no"_fnv1a32:     return "Norwegian";
        case "ps"_fnv1a32:     return "Pashto";
        case "fa"_fnv1a32:     return "Persian";
        case "pl"_fnv1a32:     return "Polish";
        case "pt"_fnv1a32:     return "Portuguese";
        case "pa"_fnv1a32:     return "Punjabi";
        case "ro"_fnv1a32:     return "Romanian";
        case "ru"_fnv1a32:     return "Russian";
        case "sm"_fnv1a32:     return "Samoan";
        case "gd"_fnv1a32:     return "Scots Gaelic";
        case "sr"_fnv1a32:     return "Serbian";
        case "st"_fnv1a32:     return "Sesotho";
        case "sn"_fnv1a32:     return "Shona";
        case "sd"_fnv1a32:     return "Sindhi";
        case "si"_fnv1a32:     return "Sinhala";
        case "sk"_fnv1a32:     return "Slovak";
        case "sl"_fnv1a32:     return "Slovenian";
        case "so"_fnv1a32:     return "Somali";
        case "es"_fnv1a32:     return "Spanish";
        case "su"_fnv1a32:     return "Sundanese";
        case "sw"_fnv1a32:     return "Swahili";
        case "sv"_fnv1a32:     return "Swedish";
        case "tg"_fnv1a32:     return "Tajik";
        case "ta"_fnv1a32:     return "Tamil";
        case "te"_fnv1a32:     return "Telugu";
        case "th"_fnv1a32:     return "Thai";
        case "tr"_fnv1a32:     return "Turkish";
        case "uk"_fnv1a32:     return "Ukrainian";
        case "ur"_fnv1a32:     return "Urdu";
        case "uz"_fnv1a32:     return "Uzbek";
        case "vi"_fnv1a32:     return "Vietnamese";
        case "cy"_fnv1a32:     return "Welsh";
        case "xh"_fnv1a32:     return "Xhosa";
        case "yi"_fnv1a32:     return "Yiddish";
        case "yo"_fnv1a32:     return "Yoruba";
        case "zu"_fnv1a32:     return "Zulu";
        case "zh"_fnv1a32:     return "Chinese (Auto)";
        case "pt-br"_fnv1a32:  return "Portuguese (Brazil)";
        case "pt-pt"_fnv1a32:  return "Portuguese (Portugal)";
        case "es-419"_fnv1a32: return "Spanish (Latin America)";
        case "es-es"_fnv1a32:  return "Spanish (Spain)";
        case "en-gb"_fnv1a32:  return "English (UK)";
        case "en-us"_fnv1a32:  return "English (US)";
        case "fr-ca"_fnv1a32:  return "French (Canada)";
        case "fr-fr"_fnv1a32:  return "French (France)";
        default:               return "Automatic";
    }
}

// clang-format off
static constexpr frozen::unordered_map<frozen::string, std::array<frozen::string, 5>, 12> lang_fonts = {
    { "hy"_s,
      {
          "arial.ttf"_s,
          "sylfaen.ttf"_s,
          "seguiemj.ttf"_s,
          "NotoSansArmenian-Regular.ttf"_s,
          "DejaVuSans.ttf"_s
      } },

    { "ar"_s,
      {
          "tahoma.ttf"_s,
          "segoeui.ttf"_s,
          "NotoSansArabic-Regular.ttf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "zh-CN"_s,
      {
          "msyh.ttc"_s,
          "simsun.ttc"_s,
          "NotoSansCJK-Regular.ttc"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "zh-TW"_s,
      {
          "msjh.ttc"_s,
          "mingliu.ttc"_s,
          "NotoSansCJK-Regular.ttc"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "ja"_s,
      {
          "meiryo.ttc"_s,
          "msgothic.ttc"_s,
          "NotoSansJP-Regular.otf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "ko"_s,
      {
          "malgun.ttf"_s,
          "gulim.ttc"_s,
          "NotoSansKR-Regular.otf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "ru"_s,
      {
          "arial.ttf"_s,
          "times.ttf"_s,
          "segoeui.ttf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "hi"_s,
      {
          "nirmala.ttf"_s,
          "mangal.ttf"_s,
          "NotoSansDevanagari-Regular.ttf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "th"_s,
      {
          "leelawui.ttf"_s,
          "cordia.ttf"_s,
          "NotoSansThai-Regular.ttf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "he"_s,
      {
          "arial.ttf"_s,
          "tahoma.ttf"_s,
          "NotoSansHebrew-Regular.ttf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s
      } },

    { "el"_s,
      {
          "arial.ttf"_s,
          "times.ttf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s,
          "NotoSans-Regular.ttf"_s
      } },

    { "default"_s,
      {
          "arial.ttf"_s,
          "segoeui.ttf"_s,
          "DejaVuSans.ttf"_s,
          "LiberationSans-Regular.ttf"_s,
          "NotoSans-Regular.ttf"_s
      } }
};
