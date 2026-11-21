#include "widget_localization.hpp"

namespace {
nlohmann::json translations = nlohmann::json::object();
std::string culture;
}

void setWidgetLocalization(const nlohmann::json& catalog) {
    if (!catalog.is_object()) return;
    const auto text = catalog.find("translations");
    if (text == catalog.end() || !text->is_object()) return;
    translations = *text;
    const auto language = catalog.find("culture");
    culture = language != catalog.end() && language->is_string() ? language->get<std::string>() : "";
}

const std::string& widgetCulture() { return culture; }

std::string localizedWidgetText(const nlohmann::json& reference) {
    if (!reference.is_object()) return {};
    const auto scope = reference.find("scope"), key = reference.find("key");
    if (scope == reference.end() || !scope->is_string() || key == reference.end() || !key->is_string()) return {};
    const auto entry = translations.find(scope->get<std::string>() + ":" + key->get<std::string>());
    if (entry == translations.end() || !entry->is_string()) return {};
    const auto& pattern = entry->get_ref<const std::string&>();
    const auto args = reference.find("arguments");
    std::string result;
    // Parse the template once so braces inside argument values stay literal.
    for (size_t i = 0; i < pattern.size();) {
        const auto end = pattern[i] == '{' ? pattern.find('}', i + 1) : std::string::npos;
        if (end != std::string::npos && args != reference.end() && args->is_object()) {
            const auto arg = args->find(pattern.substr(i + 1, end - i - 1));
            if (arg != args->end() && (arg->is_string() || arg->is_number())) {
                result += arg->is_string() ? arg->get<std::string>() : arg->dump();
                i = end + 1;
                continue;
            }
        }
        result += pattern[i++];
    }
    return result;
}
