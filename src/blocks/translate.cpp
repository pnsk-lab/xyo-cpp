// Scratch Translate extension backed by an explicitly enabled local
// PLaMo 3 Translate llama.cpp server. No model is downloaded by xyo.
#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>

#include "../core/cast.hpp"
#include "../core/sstr.hpp"
#include "../engine/block_utility.hpp"
#include "../engine/runtime.hpp"
#include "../model/target.hpp"

namespace xyo {
namespace {
std::string utf8(Value value) {
    return SStr::toUtf8(cast::toString(value)->view());
}
std::string normalized(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
std::string languageCode(std::string language) {
    language = normalized(std::move(language));
    static const std::unordered_map<std::string, std::string> names = {
        {"japanese","ja"}, {"日本語","ja"}, {"ja","ja"},
        {"english","en"}, {"英語","en"}, {"en","en"},
        {"french","fr"}, {"フランス語","fr"}, {"fr","fr"},
        {"german","de"}, {"ドイツ語","de"}, {"de","de"},
        {"spanish","es"}, {"スペイン語","es"}, {"es","es"},
        {"italian","it"}, {"イタリア語","it"}, {"it","it"},
        {"korean","ko"}, {"韓国語","ko"}, {"ko","ko"},
        {"chinese","zh"}, {"中国語","zh"}, {"zh","zh"},
        {"portuguese","pt"}, {"ポルトガル語","pt"}, {"pt","pt"},
        {"russian","ru"}, {"ロシア語","ru"}, {"ru","ru"},
        {"arabic","ar"}, {"アラビア語","ar"}, {"ar","ar"},
        {"dutch","nl"}, {"オランダ語","nl"}, {"nl","nl"},
        {"thai","th"}, {"タイ語","th"}, {"th","th"},
        {"vietnamese","vi"}, {"ベトナム語","vi"}, {"vi","vi"},
        {"indonesian","id"}, {"インドネシア語","id"}, {"id","id"},
        {"hindi","hi"}, {"ヒンディー語","hi"}, {"hi","hi"},
        {"turkish","tr"}, {"トルコ語","tr"}, {"tr","tr"},
        {"ukrainian","uk"}, {"ウクライナ語","uk"}, {"uk","uk"},
        {"polish","pl"}, {"ポーランド語","pl"}, {"pl","pl"},
        {"swedish","sv"}, {"スウェーデン語","sv"}, {"sv","sv"},
        {"finnish","fi"}, {"フィンランド語","fi"}, {"fi","fi"},
        {"danish","da"}, {"デンマーク語","da"}, {"da","da"},
        {"norwegian","no"}, {"ノルウェー語","no"}, {"no","no"}
    };
    auto found = names.find(language);
    if (found != names.end()) return found->second;
    // Allow two-character language codes from PLaMo's supported-language
    // table without accepting arbitrary user-supplied prompt fragments.
    if (language.size() == 2 && std::isalpha((unsigned char)language[0]) &&
        std::isalpha((unsigned char)language[1]))
        return language;
    if (language == "zh-cn" || language == "zh-tw") return "zh";
    return "en"; // Scratch's fallback
}
Value viewerLanguage(const ArgValues&, BlockUtility&) {
    const char* env = std::getenv("LC_ALL");
    if (!env || !*env) env = std::getenv("LANG");
    const std::string lang = normalized(env ? env : "en");
    if (lang.rfind("ja", 0) == 0)
        return Value::string(SStr::fromUtf8("日本語"));
    if (lang.rfind("es", 0) == 0)
        return Value::string(SStr::fromUtf8("Español"));
    if (lang.rfind("fr", 0) == 0)
        return Value::string(SStr::fromUtf8("Français"));
    if (lang.rfind("de", 0) == 0)
        return Value::string(SStr::fromUtf8("Deutsch"));
    return Value::string(SStr::fromUtf8("English"));
}
Value translate(const ArgValues& args, BlockUtility& utility) {
    utility.runtime()->requestTranslation(
        utility.thread, utf8(args.get("WORDS")),
        languageCode(utf8(args.get("LANGUAGE"))));
    return Value::undefined();
}
Value menu(const ArgValues& args, BlockUtility&) {
    return args.vals.empty()
        ? Value::undefined() : args.vals.begin()->second;
}
} // namespace

void registerTranslateBlocks(Runtime& runtime) {
    runtime.registerPrimitive("translate_getTranslate", translate);
    runtime.registerPrimitive("translate_getViewerLanguage", viewerLanguage);
    runtime.registerPrimitive("translate_menu_languages", menu);
}
} // namespace xyo
