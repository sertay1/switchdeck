#pragma once
#include <string>
#include <json.hpp>

void setWidgetLocalization(const nlohmann::json& catalog);
const std::string& widgetCulture();
std::string localizedWidgetText(const nlohmann::json& reference);
