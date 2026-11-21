#include "deck_state.hpp"
#include "widget_localization.hpp"
#include "config.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <cassert>
#include <sstream>
#include <array>
#include <chrono>
#include <cstring>
#include <charconv>
#include <optional>
#include <queue>
#include <set>
#include <mutex>
#include <filesystem>
#include <random>
using Json=nlohmann::json;
const Json emptyObject=Json::object(), emptyArray=Json::array();
struct TTF_Font {};
int TTF_FontLineSkip(TTF_Font*) {return 12;}
int TTF_FontHeight(TTF_Font*) {return 12;}
std::vector<SDL_Rect> drawn;
int iconDraws=0;
const int SDL_BLENDMODE_BLEND=1;
void SDL_SetRenderDrawBlendMode(SDL_Renderer*,int){}
void SDL_SetRenderDrawColor(SDL_Renderer*,int,int,int,int){}
void SDL_RenderFillRect(SDL_Renderer*,const SDL_Rect*){++iconDraws;}
template<class... T> void filledTrigonRGBA(T...){++iconDraws;}
template<class... T> void thickLineRGBA(T...){++iconDraws;}
template<class... T> void boxRGBA(T...){++iconDraws;}
template<class... T> void arcRGBA(T...){++iconDraws;}
void SDL_SetTextureColorMod(SDL_Texture*,int,int,int){}
void SDL_SetTextureAlphaMod(SDL_Texture*,int){}
void SDL_RenderCopy(SDL_Renderer*,SDL_Texture*,void*,SDL_Rect* r){drawn.push_back(*r);}
SDL_Texture* getCachedTextTexture(SDL_Renderer*,TTF_Font*,const std::string& t,int& w,int& h){static SDL_Texture tex;w=int(t.size())*6;h=12;return &tex;}
struct FakeUi {TTF_Font font; SDL_Renderer renderer;SDL_Renderer* renderer_=&renderer;
std::string fontFace;
TTF_Font* widgetFont(int,bool,const std::string& face={}){fontFace=face;return &font;} int getTextWidth(TTF_Font*,const std::string& t){return int(t.size())*6;}
std::string ellipsizeText(TTF_Font*,const std::string& s,int){return s;}
void drawText(TTF_Font*,const std::string&,int,int,SDL_Color,bool=false,int=0,bool=false,bool=false,bool=false);
void drawWrappedText(TTF_Font*,const std::string&,const SDL_Rect&,const std::string&,const std::string&,SDL_Color,bool=false);
void drawVectorIcon(const std::string&,int,int,int,SDL_Color);
static void drawFilledCircleAA(SDL_Renderer*,int,int,int,SDL_Color,SDL_Color={},int=0){++iconDraws;}
static void drawRoundedBox(SDL_Renderer*,const SDL_Rect&,int,SDL_Color){++iconDraws;}
static void drawRoundedBorder(SDL_Renderer*,const SDL_Rect&,int,int,SDL_Color){++iconDraws;}
};
std::string dynamicLabel(const Json&,const DeckButton&,int){return "12:00";}

Uint64 SDL_GetTicks64() { return 1000; }

static std::string trimText(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' ' || s.front() == '\t')) {
        s.erase(0, 1);
    }
    return s;
}

double DeckButton::getCurrentProgressMs(Uint64 nowTicks, bool clampToDuration) const {
    if (clampToDuration && progressDurationMs <= 0.0) return 0.0;
    double pos = progressPositionMs;
    if (progressRate != 0.0 && progressAnchorTicks > 0 && nowTicks > progressAnchorTicks) {
        double elapsedMs = static_cast<double>(nowTicks - progressAnchorTicks);
        pos += elapsedMs * progressRate;
    }
    if (pos < 0.0) pos = 0.0;
    if (clampToDuration && progressDurationMs > 0.0 && pos > progressDurationMs) {
        pos = progressDurationMs;
    }
    return pos;
}

float DeckButton::getCurrentProgressFraction(Uint64 nowTicks) const {
    if (progressDurationMs <= 0.0) return 0.0f;
    double pos = getCurrentProgressMs(nowTicks);
    float frac = static_cast<float>(pos / progressDurationMs);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return frac;
}

std::string DeckButton::formatDurationMs(double ms, bool roundUp) {
    if (!std::isfinite(ms) || ms < 0.0) ms = 0.0;
    const double secondsValue = std::min(ms / 1000.0, 3155760000.0);
    const auto totalSeconds = static_cast<long long>(roundUp ? std::ceil(secondsValue) : std::floor(secondsValue));
    long long hours = totalSeconds / 3600;
    long long minutes = (totalSeconds % 3600) / 60;
    long long seconds = totalSeconds % 60;
    char buf[32];
    if (hours > 0) {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", hours, minutes, seconds);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", minutes, seconds);
    }
    return std::string(buf);
}

bool DeckButton::countdownIsIdle() const {
    return !countdownIsRunning && !countdownIsFinished && progressRate <= 0 &&
           progressPositionMs <= 0 && (countdownConfiguredMs > 0 || progressDurationMs > 0);
}

std::string DeckButton::countdownReadout(Uint64 nowTicks) const {
    if (countdownIsIdle()) {
        const double duration = countdownConfiguredMs > 0 ? countdownConfiguredMs : progressDurationMs;
        return formatDurationMs(std::round(duration / 1000.0) * 1000.0);
    }
    // Macro Deck adds up to 999 ms for clients that display whole seconds by truncation.
    const double duration = countdownConfiguredMs > 0 && progressDurationMs >= countdownConfiguredMs &&
                            progressDurationMs - countdownConfiguredMs < 1000
        ? countdownConfiguredMs : progressDurationMs;
    return formatDurationMs(duration - getCurrentProgressMs(nowTicks), true);
}

static std::string leafId(const std::string& id) {
    size_t dot = id.find_last_of("./:");
    return (dot == std::string::npos) ? id : id.substr(dot + 1);
}

static std::string resolveWidgetText(const nlohmann::json& value, int depth = 0) {
    if (depth > 5) return {};
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number()) return value.dump();
    if (value.is_array()) {
        std::string result;
        for (const auto& part : value) result += resolveWidgetText(part, depth + 1);
        return result;
    }
    if (!value.is_object()) return {};
    const auto localized = value.find("$localized");
    if (localized != value.end()) {
        auto text = localizedWidgetText(*localized);
        if (!text.empty()) return text;
    }
    for (const char* key : {"text", "fallback", "value", "$localized", "default", "literal", "content", "runs"}) {
        const auto item = value.find(key);
        if (item == value.end()) continue;
        auto text = resolveWidgetText(*item, depth + 1);
        if (!text.empty()) return text;
    }
    return {};
}

std::string widgetNodeText(const nlohmann::json& node, const nlohmann::json& properties) {
    auto text = resolveWidgetText(properties);
    if (!text.empty()) return trimText(std::move(text));
    const auto fallback = node.find("fallback");
    if (fallback == node.end() || !fallback->is_object()) return {};
    const auto fallbackProps = fallback->find("properties");
    return fallbackProps != fallback->end() ? trimText(resolveWidgetText(*fallbackProps)) : std::string{};
}

std::string widgetIconName(const nlohmann::json& props) {
    const auto icon = props.find("icon");
    if (icon != props.end() && icon->is_object()) {
        return jsonStr(*icon, "name", jsonStr(*icon, "reference", jsonStr(*icon, "id")));
    }
    return jsonStr(props, "icon", jsonStr(props, "name", jsonStr(props, "reference")));
}

static void fillWeatherDetails(const nlohmann::json& node, DeckButton& btn, bool forecast, int depth = 0) {
    if (!node.is_object() || depth > 28) return;
    static const nlohmann::json empty = nlohmann::json::object();
    const auto propsIt = node.find("properties");
    const auto& props = propsIt != node.end() && propsIt->is_object() ? *propsIt : empty;
    auto key = leafId(jsonStr(node, "id", jsonStr(node, "key")));
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (key == "forecastgate" || key == "forecastdays" || key == "days" ||
        key == "hours" || key == "hourly" || key == "daily" || key == "forecastlist") forecast = true;

    if (!forecast) {
        const auto type = jsonStr(node, "type");
        const auto text = type == "ui.text" ? widgetNodeText(node, props) : std::string{};
        auto assign = [](std::string& dest, const std::string& value) {
            if (dest.empty() && !value.empty()) dest = value;
        };
        if (key == "temp" || key == "temperature" || key == "currenttemp" || key == "currenttemperature" ||
            key == "temptext" || key == "tempvalue" || key == "temperaturevalue" || key == "temperaturetext") {
            assign(btn.weatherTemp, text);
        } else if (key == "condition" || key == "conditiontext" || key == "currentcondition" ||
                   key == "conditions" || key == "conditionlabel" || key == "summary" || key == "description") {
            assign(btn.weatherCondition, text);
        } else if (key == "high" || key == "max" || key == "hightemp" || key == "maxtemp" || key == "highlabel") {
            if (!text.empty() && text.back() != ':') assign(btn.weatherHigh, text);
        } else if (key == "low" || key == "min" || key == "lowtemp" || key == "mintemp" || key == "lowlabel") {
            if (!text.empty() && text.back() != ':') assign(btn.weatherLow, text);
        } else if (key == "highlow" || key == "minmax" || key == "range" || key == "highlowtext") {
            if (text.find_first_of("0123456789") != std::string::npos) assign(btn.weatherHighLow, text);
        } else if (key == "location" || key == "locationname" || key == "city") {
            assign(btn.weatherLocation, text);
        }

        for (const auto* snapshot : {&props, &node}) {
            for (const char* field : {"weather", "snapshot", "current"}) {
                const auto nested = snapshot->find(field);
                if (nested == snapshot->end() || !nested->is_object() ||
                    (nested->contains("isAvailable") && !jsonBool(*nested, "isAvailable"))) continue;
                auto metric = [&](const char* name) {
                    auto value = nested->find(name);
                    return value == nested->end() ? std::string{} : resolveWidgetText(*value);
                };
                assign(btn.weatherTemp, metric("temperature"));
                assign(btn.weatherCondition, metric("condition"));
                assign(btn.weatherHigh, metric("high"));
                assign(btn.weatherLow, metric("low"));
                assign(btn.weatherLocation, metric("locationName"));
            }
        }
    }
    const auto children = node.find("children");
    if (children != node.end() && children->is_array()) {
        for (const auto& child : *children) fillWeatherDetails(child, btn, forecast, depth + 1);
    }
    const auto fallback = node.find("fallback");
    if (fallback != node.end()) fillWeatherDetails(*fallback, btn, forecast, depth + 1);
}

const nlohmann::json* selectResponsiveChild(const nlohmann::json& node, double widthCells, double heightCells) {
    if (!node.contains("children") || !node["children"].is_array() || node["children"].empty()) {
        return nullptr;
    }
    const auto& children = node["children"];
    if (!node.contains("properties") || !node["properties"].is_object()) {
        return &children[0];
    }
    const auto& props = node["properties"];
    if (!props.contains("variants") || !props["variants"].is_array()) {
        return &children[0];
    }
    const auto& variants = props["variants"];
    const double w = std::max(0.001, widthCells);
    const double h = std::max(0.001, heightCells);
    double aspect = w / h;
    const double eps = 1e-4;

    for (size_t i = 0; i < variants.size() && (i + 1) < children.size(); ++i) {
        const auto& cond = variants[i];
        if (!cond.is_object()) continue;
        bool match = true;
        if (cond.contains("minWidth") && cond["minWidth"].is_number()) {
            if (w < cond["minWidth"].get<double>() - eps) match = false;
        }
        if (match && cond.contains("maxWidth") && cond["maxWidth"].is_number()) {
            if (w >= cond["maxWidth"].get<double>() - eps) match = false;
        }
        if (match && cond.contains("minHeight") && cond["minHeight"].is_number()) {
            if (h < cond["minHeight"].get<double>() - eps) match = false;
        }
        if (match && cond.contains("maxHeight") && cond["maxHeight"].is_number()) {
            if (h >= cond["maxHeight"].get<double>() - eps) match = false;
        }
        if (match && cond.contains("minAspect") && cond["minAspect"].is_number()) {
            if (aspect < cond["minAspect"].get<double>() - eps) match = false;
        }
        if (match && cond.contains("maxAspect") && cond["maxAspect"].is_number()) {
            if (aspect >= cond["maxAspect"].get<double>() - eps) match = false;
        }
        if (match) {
            return &children[i + 1];
        }
    }
    return &children[0];
}

std::string formatClockDate(const std::tm& date, const std::string& format, int language) {
    char output[96]{};
    const char* pattern = nullptr;
    if (format == "date-iso") pattern = "%Y-%m-%d";
    else if (format == "date-day-first") pattern = "%d/%m/%y";
    else if (format == "date-month-first") pattern = "%m/%d/%y";
    else if (language == 1) pattern = format == "date-long" ? "%A, %B %d" : "%a, %b %d";
    if (pattern) {
        std::strftime(output, sizeof(output), pattern, &date);
        return output;
    }

    static constexpr const char* months[] = {u8"Ocak", u8"\u015eubat", u8"Mart", u8"Nisan", u8"May\u0131s",
        u8"Haziran", u8"Temmuz", u8"A\u011fustos", u8"Eyl\u00fcl", u8"Ekim", u8"Kas\u0131m", u8"Aral\u0131k"};
    static constexpr const char* shortMonths[] = {u8"Oca", u8"\u015eub", u8"Mar", u8"Nis", u8"May", u8"Haz",
        u8"Tem", u8"A\u011fu", u8"Eyl", u8"Eki", u8"Kas", u8"Ara"};
    static constexpr const char* days[] = {u8"Pazar", u8"Pazartesi", u8"Sal\u0131",
        u8"\u00c7ar\u015famba", u8"Per\u015fembe", u8"Cuma", u8"Cumartesi"};
    static constexpr const char* shortDays[] = {u8"Paz", u8"Pzt", u8"Sal", u8"\u00c7ar", u8"Per", u8"Cum", u8"Cmt"};
    const int month = std::clamp(date.tm_mon, 0, 11);
    const int day = std::clamp(date.tm_wday, 0, 6);
    const bool full = format == "date-long";
    return std::to_string(date.tm_mday) + " " + (full ? months[month] : shortMonths[month]) +
           " " + (full ? days[day] : shortDays[day]);
}

void FakeUi::drawText(TTF_Font* font, const std::string& text, int x, int y, SDL_Color color, bool center, int maxWidth, bool rightAlign, bool dropShadow, bool vCenter) {
    if (!font || text.empty() || !renderer_) return;

    std::string displayStr = (maxWidth > 0) ? ellipsizeText(font, text, maxWidth) : text;
    if (displayStr.empty()) return;

    int texW = 0, texH = 0;
    SDL_Texture* tex = getCachedTextTexture(renderer_, font, displayStr, texW, texH);
    if (!tex || texW <= 0 || texH <= 0) return;

    int drawX = x;
    int drawY = y;
    if (center) {
        drawX = x - texW / 2;
        drawY = y - texH / 2;
    } else if (rightAlign) {
        drawX = x - texW;
    }
    if (vCenter && !center) {
        drawY = y - texH / 2;
    }

    if (dropShadow) {
        SDL_SetTextureColorMod(tex, 0, 0, 0);
        SDL_SetTextureAlphaMod(tex, static_cast<Uint8>(color.a * 0.72f));
        SDL_Rect shDst = { drawX + 1, drawY + 2, texW, texH };
        SDL_RenderCopy(renderer_, tex, nullptr, &shDst);
    }

    SDL_SetTextureColorMod(tex, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(tex, color.a);
    SDL_Rect dst = { drawX, drawY, texW, texH };
    SDL_RenderCopy(renderer_, tex, nullptr, &dst);
}

void FakeUi::drawWrappedText(TTF_Font* font, const std::string& text, const SDL_Rect& area, const std::string& vPos, const std::string& hAlign, SDL_Color color, bool dropShadow) {
    if (!font || text.empty() || area.w <= 8 || area.h <= 8) return;

    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string paragraph;
    while (std::getline(stream, paragraph, '\n')) {
        std::istringstream wordStream(paragraph);
        std::string word;
        std::string currentLine;
        while (wordStream >> word) {
            std::string testLine = currentLine.empty() ? word : (currentLine + " " + word);
            if (getTextWidth(font, testLine) <= area.w) {
                currentLine = testLine;
            } else {
                if (!currentLine.empty()) {
                    lines.push_back(currentLine);
                    currentLine = word;
                } else {
                    lines.push_back(ellipsizeText(font, word, area.w));
                    currentLine.clear();
                }
            }
        }
        if (!currentLine.empty()) {
            lines.push_back(currentLine);
        }
    }

    if (lines.empty()) return;

    int lineH = TTF_FontLineSkip(font);
    if (lineH <= 0) lineH = 20;
    int maxLines = std::max(1, area.h / lineH);
    if (static_cast<int>(lines.size()) > maxLines) {
        lines.resize(maxLines);
        lines.back() = ellipsizeText(font, lines.back() + "...", area.w);
    }

    int totalH = static_cast<int>(lines.size()) * lineH;
    int startY = area.y + (area.h - totalH) / 2;
    if (vPos == "top") {
        startY = area.y + 2;
    } else if (vPos == "bottom") {
        startY = area.y + area.h - totalH - 2;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        int y = startY + static_cast<int>(i) * lineH + lineH / 2;
        if (hAlign == "left") {
            drawText(font, lines[i], area.x, y, color, false, area.w, false, dropShadow, true);
        } else if (hAlign == "right") {
            drawText(font, lines[i], area.x + area.w, y, color, false, area.w, true, dropShadow, true);
        } else {
            drawText(font, lines[i], area.x + area.w / 2, y, color, true, area.w, false, dropShadow);
        }
    }
}

void FakeUi::drawVectorIcon(const std::string& iconName, int cx, int cy, int size, SDL_Color color) {
    if (!renderer_) return;

    const auto named = [&](const std::string& name) {
        return iconName == name || iconName == "player-" + name ||
               iconName == "player-" + name + "-filled";
    };
    const bool previous = named("track-prev") || named("skip-back") || named("previous") || named("skip-previous");
    const bool next = named("track-next") || named("skip-forward") || named("next") || named("skip-next");
    if (named("play") || named("pause") || previous || next) {
        if (size < 3) return;
        const int half = std::max(2, size / 3);
        const int stroke = std::max(1, size / 8);
        SDL_SetRenderDrawColor(renderer_, color.r, color.g, color.b, color.a);
        if (named("pause")) {
            const SDL_Rect left{cx - half, cy - half, stroke * 2, half * 2};
            const SDL_Rect right{cx + half - stroke * 2, cy - half, stroke * 2, half * 2};
            SDL_RenderFillRect(renderer_, &left);
            SDL_RenderFillRect(renderer_, &right);
        } else {
            const int direction = previous ? -1 : 1;
            const int side = cx - direction * half;
            const int tip = cx + direction * half;
            filledTrigonRGBA(renderer_, side, cy - half, side, cy + half, tip, cy,
                             color.r, color.g, color.b, color.a);
            if (previous || next) {
                const SDL_Rect bar{tip - (previous ? stroke : 0), cy - half, stroke, half * 2};
                SDL_RenderFillRect(renderer_, &bar);
            }
        }
        return;
    }

    if (iconName == "settings") {
        int rOuter = size / 2;
        int rInner = std::max(3, size / 4);
        for (int i = 0; i < 4; ++i) {
            double angle = i * (3.14159265 / 4.0);
            int x1 = cx + static_cast<int>(std::round(std::cos(angle) * (rOuter + 2)));
            int y1 = cy + static_cast<int>(std::round(std::sin(angle) * (rOuter + 2)));
            int x2 = cx - static_cast<int>(std::round(std::cos(angle) * (rOuter + 2)));
            int y2 = cy - static_cast<int>(std::round(std::sin(angle) * (rOuter + 2)));
            thickLineRGBA(renderer_, x1, y1, x2, y2, 3, color.r, color.g, color.b, color.a);
        }
        drawFilledCircleAA(renderer_, cx, cy, rOuter - 1, color);
        drawFilledCircleAA(renderer_, cx, cy, rInner, {26, 26, 26, 255});
    } else if (iconName == "refresh") {
        int r = size / 2;
        arcRGBA(renderer_, cx, cy, r, 35, 330, color.r, color.g, color.b, color.a);
        arcRGBA(renderer_, cx, cy, r - 1, 35, 330, color.r, color.g, color.b, color.a);
        filledTrigonRGBA(renderer_, cx + r - 3, cy - 5, cx + r + 3, cy - 5, cx + r, cy + 1, color.r, color.g, color.b, color.a);
    } else if (iconName == "back") {
        int s = size / 2;
        thickLineRGBA(renderer_, cx + s / 2, cy - s, cx - s / 2, cy, 2, color.r, color.g, color.b, color.a);
        thickLineRGBA(renderer_, cx - s / 2, cy, cx + s / 2, cy + s, 2, color.r, color.g, color.b, color.a);
        thickLineRGBA(renderer_, cx - s / 2, cy, cx + s, cy, 2, color.r, color.g, color.b, color.a);
    } else if (iconName == "folder") {
        SDL_Rect body = { cx - size / 2, cy - size / 3 + 3, size, (size * 2) / 3 };
        SDL_Rect tab  = { cx - size / 2, cy - size / 3 - 2, size / 2 - 2, 7 };
        drawRoundedBox(renderer_, tab, 3, color);
        drawRoundedBox(renderer_, body, 4, color);
    } else if (iconName == "music") {
        int r = std::max(3, size / 4);
        drawFilledCircleAA(renderer_, cx - r, cy + r, r, color);
        thickLineRGBA(renderer_, cx, cy + r, cx, cy - size / 2, 2, color.r, color.g, color.b, color.a);
        thickLineRGBA(renderer_, cx, cy - size / 2, cx + r + 2, cy - size / 3, 3, color.r, color.g, color.b, color.a);
    } else if (iconName == "cpu") {
        int coreSz = std::max(6, (size * 3) / 5);
        SDL_Rect core = { cx - coreSz / 2, cy - coreSz / 2, coreSz, coreSz };
        drawRoundedBox(renderer_, core, 2, color);
        drawRoundedBorder(renderer_, core, 2, 1, {255, 255, 255, 160});
        int pinLen = std::max(2, size / 7);
        int step = std::max(2, coreSz / 3);
        for (int p = -coreSz / 3; p <= coreSz / 3; p += step) {
            thickLineRGBA(renderer_, cx + p, core.y - pinLen, cx + p, core.y, 1, color.r, color.g, color.b, color.a);
            thickLineRGBA(renderer_, cx + p, core.y + core.h, cx + p, core.y + core.h + pinLen, 1, color.r, color.g, color.b, color.a);
            thickLineRGBA(renderer_, core.x - pinLen, cy + p, core.x, cy + p, 1, color.r, color.g, color.b, color.a);
            thickLineRGBA(renderer_, core.x + core.w, cy + p, core.x + core.w + pinLen, cy + p, 1, color.r, color.g, color.b, color.a);
        }
    } else if (iconName == "ram") {
        int w = size;
        int h = std::max(6, (size * 5) / 10);
        SDL_Rect stick = { cx - w / 2, cy - h / 2, w, h };
        drawRoundedBox(renderer_, stick, 2, color);
        int chipW = std::max(2, w / 5);
        int chipH = std::max(3, h - 4);
        int step = std::max(3, w / 3);
        for (int offset = -w / 3; offset <= w / 3; offset += step) {
            SDL_Rect chip = { cx + offset - chipW / 2, cy - chipH / 2, chipW, chipH };
            drawRoundedBox(renderer_, chip, 1, {25, 30, 42, 255});
        }
        thickLineRGBA(renderer_, stick.x + 2, stick.y + stick.h, stick.x + w - 2, stick.y + stick.h, 2, 234, 179, 8, 220);
    } else if (iconName == "gpu") {
        int w = size;
        int h = std::max(7, (size * 6) / 10);
        SDL_Rect card = { cx - w / 2 + 2, cy - h / 2, w - 4, h };
        drawRoundedBox(renderer_, card, 2, color);
        thickLineRGBA(renderer_, card.x - 2, card.y - 2, card.x - 2, card.y + h + 2, 2, 200, 210, 225, 255);
        int fanR = std::max(3, h / 3);
        drawFilledCircleAA(renderer_, cx, cy, fanR, {20, 25, 35, 255});
        drawFilledCircleAA(renderer_, cx, cy, 2, color);
        thickLineRGBA(renderer_, card.x + 4, card.y + h, card.x + w / 2, card.y + h, 2, 234, 179, 8, 220);
    } else if (iconName == "battery-charging" || iconName == "zap") {
        const int x = std::max(2, size / 4);
        const int y = std::max(3, size / 2);
        if (iconName == "battery-charging") {
            drawRoundedBorder(renderer_, {cx - size / 2, cy - y * 2 / 3, size - 1, y * 4 / 3}, 2, 1, color);
        }
        filledTrigonRGBA(renderer_, cx + 1, cy - y, cx - x, cy + 1, cx + x, cy + 1,
                         color.r, color.g, color.b, color.a);
        filledTrigonRGBA(renderer_, cx - 1, cy + y, cx + x, cy - 1, cx - x, cy - 1,
                         color.r, color.g, color.b, color.a);
    } else if (iconName == "battery") {
        int bw = std::max(8, (size * 7) / 10);
        int bh = std::max(6, (size * 4) / 9);
        SDL_Rect body = { cx - bw / 2, cy - bh / 2, bw, bh };
        drawRoundedBorder(renderer_, body, 2, 2, color);
        SDL_Rect fill = { body.x + 3, body.y + 3, std::max(2, (bw - 6) * 2 / 3), std::max(2, bh - 6) };
        drawRoundedBox(renderer_, fill, 1, color);
        boxRGBA(renderer_, body.x + bw, cy - 2, body.x + bw + 2, cy + 2, color.r, color.g, color.b, color.a);
    } else if (iconName.find("mouse") != std::string::npos) {
        int mw = std::max(8, size / 2);
        int mh = std::max(10, (size * 4) / 5);
        drawRoundedBorder(renderer_, {cx - mw / 2, cy - mh / 2, mw, mh}, mw / 2, 2, color);
        thickLineRGBA(renderer_, cx, cy - mh / 2 + 2, cx, cy - 1, 2, color.r, color.g, color.b, color.a);
    } else if (iconName == "device-laptop" || iconName == "device-desktop") {
        const int w = std::max(6, size - 2);
        const int h = std::max(4, size * 3 / 5);
        drawRoundedBorder(renderer_, {cx - w / 2, cy - h / 2 - 1, w, h}, 2, 1, color);
        const int foot = iconName == "device-laptop" ? size / 2 : size / 4;
        if (iconName == "device-desktop")
            thickLineRGBA(renderer_, cx, cy + h / 2 - 1, cx, cy + h / 2 + 2, 1, color.r, color.g, color.b, color.a);
        thickLineRGBA(renderer_, cx - foot, cy + h / 2 + 2, cx + foot, cy + h / 2 + 2, 2,
                      color.r, color.g, color.b, color.a);
    } else if (iconName == "device-watch") {
        const int w = std::max(5, size * 3 / 5);
        drawRoundedBorder(renderer_, {cx - w / 2, cy - w / 2, w, w}, 2, 1, color);
        thickLineRGBA(renderer_, cx, cy - size / 2, cx, cy - w / 2, 2, color.r, color.g, color.b, color.a);
        thickLineRGBA(renderer_, cx, cy + w / 2, cx, cy + size / 2, 2, color.r, color.g, color.b, color.a);
    } else if (iconName == "device-speaker") {
        const int w = std::max(5, size * 3 / 5);
        drawRoundedBorder(renderer_, {cx - w / 2, cy - size / 2, w, size}, 2, 1, color);
        drawFilledCircleAA(renderer_, cx, cy - size / 4, 1, color);
        drawFilledCircleAA(renderer_, cx, cy + size / 5, std::max(1, w / 4), color);
    } else if (iconName.find("headphone") != std::string::npos) {
        int r = std::max(5, size / 3);
        arcRGBA(renderer_, cx, cy, r, 180, 360, color.r, color.g, color.b, color.a);
        arcRGBA(renderer_, cx, cy, r - 1, 180, 360, color.r, color.g, color.b, color.a);
        drawRoundedBox(renderer_, {cx - r - 2, cy - 1, 4, 7}, 2, color);
        drawRoundedBox(renderer_, {cx + r - 2, cy - 1, 4, 7}, 2, color);
    } else if (iconName.find("gamepad") != std::string::npos || iconName.find("controller") != std::string::npos) {
        int gw = std::max(10, (size * 4) / 5);
        int gh = std::max(6, size / 2);
        drawRoundedBox(renderer_, {cx - gw / 2, cy - gh / 2, gw, gh}, 4, color);
        drawFilledCircleAA(renderer_, cx - gw / 4, cy, 2, {20, 24, 32, 255});
        drawFilledCircleAA(renderer_, cx + gw / 4, cy, 2, {20, 24, 32, 255});
    } else if (iconName.find("keyboard") != std::string::npos) {
        int kw = std::max(10, (size * 4) / 5);
        int kh = std::max(6, size / 2);
        drawRoundedBorder(renderer_, {cx - kw / 2, cy - kh / 2, kw, kh}, 2, 2, color);
        boxRGBA(renderer_, cx - kw / 3, cy + 1, cx + kw / 3, cy + 2, color.r, color.g, color.b, color.a);
    } else if (iconName.find("phone") != std::string::npos || iconName.find("tablet") != std::string::npos || iconName.find("watch") != std::string::npos) {
        int pw = std::max(7, size / 2);
        int ph = std::max(10, (size * 4) / 5);
        drawRoundedBorder(renderer_, {cx - pw / 2, cy - ph / 2, pw, ph}, 2, 2, color);
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
}

static void copyRuntimeState(const DeckButton& src, DeckButton& dst) {
    if (!src.sessionId.empty()) dst.sessionId = src.sessionId;
    if (src.revision > 0) dst.revision = src.revision;
    if (!src.uiTree.is_null()) dst.uiTree = src.uiTree;
    if (!src.primaryNodeId.empty()) dst.primaryNodeId = src.primaryNodeId;
    if (!src.primaryNodeEvents.empty()) dst.primaryNodeEvents = src.primaryNodeEvents;
    if (!src.sliderNodeId.empty()) dst.sliderNodeId = src.sliderNodeId;
    if (!src.subButtons.empty()) dst.subButtons = src.subButtons;
    if (!src.treeResourceIds.empty()) dst.treeResourceIds = src.treeResourceIds;

    // Slider state
    dst.sliderLevel = src.sliderLevel;
    dst.visualSliderLevel = src.visualSliderLevel;
    if (dst.sliderShowValue && !src.sliderValueText.empty()) dst.sliderValueText = src.sliderValueText;
    dst.lastLocalSlideTick = src.lastLocalSlideTick;
    dst.sliderIsVertical = src.sliderIsVertical;

    // Progress / Timeline state
    dst.progressPositionMs = src.progressPositionMs;
    dst.progressDurationMs = src.progressDurationMs;
    dst.progressRate = src.progressRate;
    dst.progressAnchorStr = src.progressAnchorStr;
    dst.progressAnchorOffsetMs = src.progressAnchorOffsetMs;
    dst.progressAnchorTicks = src.progressAnchorTicks;

    // Countdown state
    dst.countdownRingLevel = src.countdownRingLevel;
    dst.countdownIsFinished = src.countdownIsFinished;
    dst.countdownIsPaused = src.countdownIsPaused;
    dst.countdownIsRunning = src.countdownIsRunning;
    if (!src.countdownTimeText.empty()) dst.countdownTimeText = src.countdownTimeText;
    if (!src.countdownCaption.empty()) dst.countdownCaption = src.countdownCaption;

    // Stopwatch state
    if (!src.stopwatchTimeText.empty()) dst.stopwatchTimeText = src.stopwatchTimeText;
    dst.stopwatchIsRunning = src.stopwatchIsRunning;
    dst.stopwatchIsPaused = src.stopwatchIsPaused;

    // Weather state
    if (!src.weatherTemp.empty()) dst.weatherTemp = src.weatherTemp;
    if (!src.weatherCondition.empty()) dst.weatherCondition = src.weatherCondition;
    if (!src.weatherLocation.empty()) dst.weatherLocation = src.weatherLocation;
    if (!src.weatherHighLow.empty()) dst.weatherHighLow = src.weatherHighLow;
    if (!src.weatherForecast.empty()) dst.weatherForecast = src.weatherForecast;
    if (!src.weatherStyle.empty()) dst.weatherStyle = src.weatherStyle;

    // Gauges state
    if (!src.gauges.empty()) dst.gauges = src.gauges;
    dst.gaugeIsArc = src.gaugeIsArc;

    // Battery state
    dst.batteryLevel = src.batteryLevel;
    dst.batteryIsCharging = src.batteryIsCharging;
    if (!src.batteryText.empty()) dst.batteryText = src.batteryText;
    if (!src.batteryStateText.empty()) dst.batteryStateText = src.batteryStateText;
    if (!src.batteryDevices.empty()) dst.batteryDevices = src.batteryDevices;

    // Calendar state
    if (!src.calendarMonthText.empty()) dst.calendarMonthText = src.calendarMonthText;
    if (!src.calendarDayText.empty()) dst.calendarDayText = src.calendarDayText;
    if (!src.calendarWeekdayText.empty()) dst.calendarWeekdayText = src.calendarWeekdayText;
    if (!src.calendarAgendaHeading.empty()) dst.calendarAgendaHeading = src.calendarAgendaHeading;
    if (!src.calendarEvents.empty()) dst.calendarEvents = src.calendarEvents;
    if (!src.calendarNextDate.empty()) dst.calendarNextDate = src.calendarNextDate;
    if (!src.calendarNextTitle.empty()) dst.calendarNextTitle = src.calendarNextTitle;
    if (!src.calendarNextWhen.empty()) dst.calendarNextWhen = src.calendarNextWhen;
    if (!src.calendarNextLocation.empty()) dst.calendarNextLocation = src.calendarNextLocation;

    // MusicPlayer state
    if (!src.musicCoverStyle.empty()) dst.musicCoverStyle = src.musicCoverStyle;
    if (!src.musicProviderName.empty()) dst.musicProviderName = src.musicProviderName;
    if (!src.musicSourceName.empty()) dst.musicSourceName = src.musicSourceName;
    if (!src.musicBadgeText.empty()) dst.musicBadgeText = src.musicBadgeText;
    if (!src.musicTitle.empty()) dst.musicTitle = src.musicTitle;
    if (!src.musicArtist.empty()) dst.musicArtist = src.musicArtist;
    if (!src.musicAlbum.empty()) dst.musicAlbum = src.musicAlbum;
    dst.musicIsPlaying = src.musicIsPlaying;
    dst.musicIsPaused = src.musicIsPaused;
    dst.musicHasWarning = src.musicHasWarning;
    dst.musicHasTrack = src.musicHasTrack;

    // Accent / Colors
    if (src.hasAccentColor) {
        dst.hasAccentColor = true;
        dst.accentR = src.accentR;
        dst.accentG = src.accentG;
        dst.accentB = src.accentB;
    }

    // Graph history
    if (!src.graphHistory.empty()) dst.graphHistory = src.graphHistory;

    // Resource & Textures
    if (dst.resourceId.empty() && !src.resourceId.empty()) dst.resourceId = src.resourceId;
    if (!src.loadedResourceId.empty()) dst.loadedResourceId = src.loadedResourceId;
}

static std::optional<std::array<int, 3>> parseReleaseVersion(const std::string& tag) {
    std::array<int, 3> version{};
    const char* part = tag.data() + (!tag.empty() && tag.front() == 'v');
    const char* end = tag.data() + tag.size();
    for (size_t i = 0; i < version.size(); ++i) {
        const auto result = std::from_chars(part, end, version[i]);
        if (result.ec != std::errc{} || version[i] < 0) return std::nullopt;
        if (i + 1 == version.size()) return result.ptr == end ? std::optional{version} : std::nullopt;
        if (result.ptr == end || *result.ptr != '.') return std::nullopt;
        part = result.ptr + 1;
    }
    return std::nullopt;
}


struct TestTimeout { long tv_sec, tv_usec; };
constexpr int SOL_SOCKET = 0, SO_RCVTIMEO = 0;
void setsockopt(int, int, int, const void*, size_t) {}
std::string serverBytes;
int readSocket(int, char* data, size_t requested, int) {
    const auto count = std::min(requested, serverBytes.size());
    std::memcpy(data, serverBytes.data(), count);
    serverBytes.erase(0, count);
    return static_cast<int>(count);
}
struct ConnectionTest {
    bool running_ = true, awaitingPong_ = true;
    uint64_t pingSequence_ = 1;
    int pingMs_ = -1;
    std::array<int, 3> pingSamples_{};
    size_t nextPingSample_ = 0;
    std::chrono::steady_clock::time_point lastPingSentTime_;
    bool sendFrame(int, uint8_t, const uint8_t*, size_t) { return true; }
    bool readFrame(int, std::string&, int);
    void pong(uint64_t sequence, int millisecondsAgo) {
        serverBytes = {char(0x8A), char(sizeof(sequence))};
        serverBytes.append(reinterpret_cast<const char*>(&sequence), sizeof(sequence));
        lastPingSentTime_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(millisecondsAgo);
        std::string message;
        assert(readFrame(0, message, 0) && message.empty());
    }
};

bool ConnectionTest::readFrame(int sock, std::string& outMessage, int headerTimeoutMs) {
    outMessage.clear();
    constexpr size_t maxMessageBytes = 4 * 1024 * 1024;
    TestTimeout headerTimeout{headerTimeoutMs / 1000, (headerTimeoutMs % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &headerTimeout, sizeof(headerTimeout));
    uint8_t header[2];
    if (readSocket(sock, reinterpret_cast<char*>(header), 1, 0) != 1) return false;

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    const auto receiveExact = [&](void* destination, size_t remaining) {
        auto* bytes = static_cast<char*>(destination);
        while (remaining > 0 && running_) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
            if (us <= 0) return false;
            TestTimeout timeout{static_cast<long>(us / 1000000), static_cast<long>(us % 1000000)};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            const auto received = readSocket(sock, bytes, remaining, 0);
            if (received <= 0) return false;
            bytes += received;
            remaining -= static_cast<size_t>(received);
        }
        return remaining == 0;
    };
    if (!receiveExact(header + 1, 1)) return false;

    uint8_t messageOpcode = 0;
    while (running_) {
        const bool final = (header[0] & 0x80) != 0;
        const uint8_t opcode = header[0] & 0x0F;
        const bool control = (opcode & 0x08) != 0;
        if ((header[0] & 0x70) || (header[1] & 0x80)) return false;
        uint64_t length = header[1] & 0x7F;
        if (control && (!final || length > 125)) return false;
        if (length >= 126) {
            const size_t extendedSize = length == 126 ? 2 : 8;
            uint8_t extended[8];
            if (!receiveExact(extended, extendedSize)) return false;
            length = 0;
            for (size_t i = 0; i < extendedSize; ++i) length = (length << 8) | extended[i];
            if (length < (extendedSize == 2 ? 126 : 65536)) return false;
        }

        if (control) {
            uint8_t payload[125];
            if (!receiveExact(payload, static_cast<size_t>(length))) return false;
            if (opcode == 0x8) {
                if (length != 1) sendFrame(sock, 0x8, payload, static_cast<size_t>(length));
                return false;
            }
            if (opcode == 0x9) {
                if (!sendFrame(sock, 0xA, payload, static_cast<size_t>(length))) return false;
            } else if (opcode == 0xA) {
                if (awaitingPong_ && length == sizeof(pingSequence_) &&
                    std::memcmp(payload, &pingSequence_, sizeof(pingSequence_)) == 0) {
                    awaitingPong_ = false;
                    const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastPingSentTime_).count();
                    const int sample = std::max(1, static_cast<int>(rtt));
                    if (pingSamples_[0] == 0) pingSamples_.fill(sample);
                    pingSamples_[nextPingSample_] = sample;
                    nextPingSample_ = (nextPingSample_ + 1) % pingSamples_.size();
                    auto ordered = pingSamples_;
                    std::sort(ordered.begin(), ordered.end());
                    pingMs_ = ordered[1];
                }
            } else {
                return false;
            }
            if (messageOpcode == 0) return true;
        } else {
            if (opcode == 0x1 || opcode == 0x2) {
                if (messageOpcode != 0) return false;
                messageOpcode = opcode;
            } else if (opcode != 0 || messageOpcode == 0) {
                return false;
            }
            if (length > maxMessageBytes - outMessage.size()) return false;
            const auto offset = outMessage.size();
            outMessage.resize(offset + static_cast<size_t>(length));
            if (!receiveExact(outMessage.data() + offset, static_cast<size_t>(length))) return false;
            if (final) {
                if (messageOpcode == 0x2) outMessage.clear();
                return true;
            }
        }
        if (!receiveExact(header, sizeof(header))) return false;
    }
    return false;
}


struct WidgetConnectionTest {
    enum class WsState { Connected };
    WsState state_ = WsState::Connected;
    struct WidgetSessionAttempt {
        std::string requestId;
        std::chrono::steady_clock::time_point deadline;
    };
    std::set<std::string> displayedWidgets_;
    std::map<std::string, WidgetSessionAttempt> widgetSessionAttempts_;
    std::map<std::string, std::string> sessionToWidget_;
    std::queue<std::string> outgoingQueue_, incomingQueue_;
    std::mutex queueMutex_;
    uint64_t reqSeq_ = 0;
    void syncWidgetSessions();
    void queueSessionAttach(const std::string&, const std::string&);
    void queueSessionClose(const std::string&);
    void retryWidgetSession(const std::string&, const std::string&);
    void resyncWidgetSession(const std::string&);
    bool hasWidgetSession(const std::string&);
    bool handleUiSessionMessage(const Json&);
    bool isConnected() const { return true; }
    std::string actionError;
    void reportActionError(const std::string& error) { actionError = error; }
    void sendUiEvent(const std::string&, const std::string&, const std::string&, const Json&, int);
    Json nextRequest() {
        assert(!outgoingQueue_.empty());
        auto request = Json::parse(outgoingQueue_.front());
        outgoingQueue_.pop();
        return request;
    }
    void expire(const std::string& widget) { widgetSessionAttempts_.at(widget).deadline = {}; }
};

void WidgetConnectionTest::syncWidgetSessions() {
    const auto now = std::chrono::steady_clock::now();
    std::set<std::string> sessions;
    for (auto it = sessionToWidget_.begin(); it != sessionToWidget_.end();) {
        const auto attempt = widgetSessionAttempts_.find(it->second);
        if (attempt != widgetSessionAttempts_.end() && now >= attempt->second.deadline) {
            const auto sessionId = it->first;
            const auto widgetId = it->second;
            ++it;
            retryWidgetSession(widgetId, sessionId);
        } else {
            sessions.insert(it->second);
            ++it;
        }
    }
    for (auto it = widgetSessionAttempts_.begin(); it != widgetSessionAttempts_.end();) {
        if (!displayedWidgets_.count(it->first) || now >= it->second.deadline) {
            it = widgetSessionAttempts_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& widgetId : displayedWidgets_) {
        if (widgetId.empty() || sessions.count(widgetId) || widgetSessionAttempts_.count(widgetId)) continue;
        const std::string id = "open-" + std::to_string(++reqSeq_);
        widgetSessionAttempts_[widgetId] = {id, now + std::chrono::seconds(5)};
        nlohmann::json request = {
            {"protocolVersion", 1}, {"kind", "request"}, {"type", "OpenWidgetUiSession"}, {"id", id},
            {"payload", nlohmann::json::array({{{"widgetId", widgetId}}})}
        };
        outgoingQueue_.push(request.dump());
    }
}

void WidgetConnectionTest::queueSessionAttach(const std::string& widgetId, const std::string& sessionId) {
    const std::string id = "attach-" + std::to_string(++reqSeq_);
    widgetSessionAttempts_[widgetId] = {id, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
    nlohmann::json request = {
        {"protocolVersion", 1}, {"kind", "request"}, {"type", "AttachUiSession"}, {"id", id},
        {"payload", nlohmann::json::array({{{"sessionId", sessionId}}})}
    };
    outgoingQueue_.push(request.dump());
}

void WidgetConnectionTest::queueSessionClose(const std::string& sessionId) {
    nlohmann::json request = {
        {"protocolVersion", 1}, {"kind", "request"}, {"type", "CloseUiSession"},
        {"id", "close-" + std::to_string(++reqSeq_)}, {"payload", nlohmann::json::array({sessionId})}
    };
    outgoingQueue_.push(request.dump());
}

void WidgetConnectionTest::retryWidgetSession(const std::string& widgetId, const std::string& sessionId) {
    if (!sessionId.empty()) {
        queueSessionClose(sessionId);
        sessionToWidget_.erase(sessionId);
        const nlohmann::json ended = {{"_internal", "MD3_WIDGET_SESSION_ENDED"}, {"sessionId", sessionId}};
        incomingQueue_.push(ended.dump());
    }
    widgetSessionAttempts_[widgetId] = {{}, std::chrono::steady_clock::now() + std::chrono::seconds(2)};
}

void WidgetConnectionTest::resyncWidgetSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    const auto session = sessionToWidget_.find(sessionId);
    if (state_ != WsState::Connected || session == sessionToWidget_.end() ||
        widgetSessionAttempts_.count(session->second)) return;
    queueSessionAttach(session->second, sessionId);
}

bool WidgetConnectionTest::hasWidgetSession(const std::string& widgetId) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return std::any_of(sessionToWidget_.begin(), sessionToWidget_.end(), [&](const auto& session) {
        return session.second == widgetId;
    });
}

bool WidgetConnectionTest::handleUiSessionMessage(const nlohmann::json& envelope) {
    const auto kind = jsonStr(envelope, "kind");
    const auto type = jsonStr(envelope, "type");
    if (kind == "message") {
        const auto body = envelope.find("payload");
        if (body == envelope.end() || !body->is_object()) return false;
        if (type == "UiSessionTreeUpdatedEvent" && body->contains("tree")) {
            const auto session = sessionToWidget_.find(jsonStr(*body, "sessionId"));
            if (session != sessionToWidget_.end()) widgetSessionAttempts_.erase(session->second);
        } else if (type == "UiSessionClosedEvent" || type == "UiSessionInvalidatedEvent") {
            const auto session = sessionToWidget_.find(jsonStr(*body, "sessionId"));
            if (session != sessionToWidget_.end()) {
                widgetSessionAttempts_[session->second] = {{}, std::chrono::steady_clock::now() + std::chrono::seconds(2)};
                sessionToWidget_.erase(session);
            }
        }
        return false;
    }
    if (kind != "response" && kind != "error") return false;

    const auto id = jsonStr(envelope, "correlationId", jsonStr(envelope, "id"));
    const bool opening = id.rfind("open-", 0) == 0;
    const bool uiEvent = id.rfind("ev-", 0) == 0;
    if (!uiEvent && !opening && id.rfind("attach-", 0) != 0) return false;
    const auto payload = envelope.find("payload");
    const nlohmann::json* body = payload == envelope.end() ? nullptr : &*payload;
    if (body && body->is_array()) body = body->empty() ? nullptr : &(*body)[0];
    const bool accepted = kind == "response" && body && jsonBool(*body, "accepted", false);
    if (uiEvent) {
        if (accepted) return true;
        const auto separator = id.find(':');
        if (separator == std::string::npos) return true;
        const auto session = sessionToWidget_.find(id.substr(separator + 1));
        if (session == sessionToWidget_.end()) return true;
        const bool synchronizing = widgetSessionAttempts_.count(session->second) != 0;
        if (synchronizing && id.rfind("ev-quiet-", 0) == 0) return true;

        const auto error = envelope.find("error");
        const auto code = body ? jsonStr(*body, "code")
                               : error == envelope.end() ? std::string{} : jsonStr(*error, "code");
        reportActionError(code == "HOST_LOCKED" ? "host-locked" : "widget-rejected");
        // Refresh before the next press. Repeating the original command could trigger it twice.
        if (code != "HOST_LOCKED" && !synchronizing) {
            queueSessionAttach(session->second, session->first);
        }
        return true;
    }

    const auto attempt = std::find_if(widgetSessionAttempts_.begin(), widgetSessionAttempts_.end(),
        [&](const auto& entry) { return entry.second.requestId == id; });
    const std::string sessionId = body ? jsonStr(*body, "sessionId") : std::string{};

    if (attempt == widgetSessionAttempts_.end()) {
        // A page change or timeout can retire an open request before the reply arrives.
        if (opening && !sessionId.empty() && !sessionToWidget_.count(sessionId)) queueSessionClose(sessionId);
        return true;
    }
    const auto widgetId = attempt->first;
    if (opening && accepted && !sessionId.empty()) {
        sessionToWidget_[sessionId] = widgetId;
        const nlohmann::json opened = {
            {"_internal", "MD3_WIDGET_SESSION"}, {"widgetId", widgetId}, {"sessionId", sessionId}
        };
        incomingQueue_.push(opened.dump());
        queueSessionAttach(widgetId, sessionId);
    } else if (opening) {
        retryWidgetSession(widgetId, {});
    } else if (!accepted) {
        const auto session = std::find_if(sessionToWidget_.begin(), sessionToWidget_.end(),
            [&](const auto& entry) { return entry.second == widgetId; });
        retryWidgetSession(widgetId, session == sessionToWidget_.end() ? std::string{} : session->first);
    }
    return true;
}

void WidgetConnectionTest::sendUiEvent(const std::string& sessionId, const std::string& nodeId, const std::string& eventName, const nlohmann::json& data, int revision) {
    if (sessionId.empty() || nodeId.empty()) return;
    if (!isConnected()) {
        reportActionError("disconnected");
        return;
    }
    nlohmann::json evObj;
    evObj["sessionId"] = sessionId;
    evObj["nodeId"] = nodeId;
    evObj["name"] = eventName;
    if (!data.is_null()) {
        evObj["data"] = data;
    }
    evObj["revision"] = revision;

    nlohmann::json req;
    req["protocolVersion"] = 1;
    req["kind"] = "request";
    req["type"] = "SendUiEvent";
    const bool quiet = eventName == "adjust" || eventName == "press-end";
    req["id"] = (quiet ? "ev-quiet-" : "ev-") + std::to_string(++reqSeq_) + ":" + sessionId;
    req["payload"] = nlohmann::json::array({ evObj });
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (state_ == WsState::Connected && sessionToWidget_.count(sessionId)) {
        outgoingQueue_.push(req.dump());
    } else if (!quiet) {
        reportActionError(state_ == WsState::Connected ? "widget-reconnecting" : "disconnected");
    }
}


void testWidgetSessionRecovery() {
    WidgetConnectionTest connection;
    connection.displayedWidgets_ = {"widget"};
    connection.syncWidgetSessions();
    const auto reply = [&](const Json& request, bool accepted, const char* sid) {
        assert(connection.handleUiSessionMessage({
            {"kind", "response"}, {"correlationId", request.at("id")},
            {"payload", {{"accepted", accepted}, {"sessionId", sid}}}
        }));
    };
    const auto original = connection.nextRequest();
    assert(original.at("type") == "OpenWidgetUiSession");
    connection.expire("widget");
    connection.syncWidgetSessions();
    const auto retry = connection.nextRequest();
    assert(retry.at("id") != original.at("id"));
    reply(original, true, "late");
    assert(connection.nextRequest().at("type") == "CloseUiSession");
    assert(!connection.hasWidgetSession("widget"));

    reply(retry, true, "first");
    auto attach = connection.nextRequest();
    assert(attach.at("type") == "AttachUiSession");
    reply(attach, false, "");
    assert(!connection.hasWidgetSession("widget"));
    assert(connection.nextRequest().at("payload").at(0) == "first");
    connection.sendUiEvent("first", "play", "press", nullptr, 1);
    assert(connection.actionError == "widget-reconnecting");
    connection.actionError.clear();
    connection.syncWidgetSessions();
    assert(connection.outgoingQueue_.empty());

    connection.expire("widget");
    connection.syncWidgetSessions();
    reply(connection.nextRequest(), true, "reopened");
    attach = connection.nextRequest();
    reply(attach, true, "reopened");
    const Json tree = {{"kind", "message"}, {"type", "UiSessionTreeUpdatedEvent"},
        {"payload", {{"sessionId", "reopened"}, {"tree", Json::object()}}}};
    assert(!connection.handleUiSessionMessage(tree));
    assert(connection.widgetSessionAttempts_.empty());
    assert(!connection.handleUiSessionMessage({{"kind", "message"}, {"type", "WidgetStateInvalidatedEvent"},
        {"payload", {{"widgetId", "widget"}}}}));
    assert(connection.hasWidgetSession("widget"));
    connection.syncWidgetSessions();
    assert(connection.outgoingQueue_.empty());

    connection.sendUiEvent("reopened", "action", "press", nullptr, 2);
    connection.sendUiEvent("reopened", "volume", "change", 0.5, 2);
    connection.sendUiEvent("reopened", "spotify-next", "press", nullptr, 2);
    assert(connection.nextRequest().at("payload").at(0).at("nodeId") == "action");
    assert(connection.nextRequest().at("payload").at(0).at("data") == 0.5);
    const auto nextTrack = connection.nextRequest();
    assert(nextTrack.at("payload").at(0).at("nodeId") == "spotify-next");
    reply(nextTrack, true, "");
    assert(connection.actionError.empty() && connection.outgoingQueue_.empty());

    connection.sendUiEvent("reopened", "volume", "change", 0.75, 2);
    const auto changeVolume = connection.nextRequest();
    assert(connection.handleUiSessionMessage({{"kind", "response"}, {"correlationId", changeVolume.at("id")},
                                              {"payload", {{"accepted", false}, {"code", "HOST_LOCKED"}}}}));
    assert(connection.actionError == "host-locked" && connection.outgoingQueue_.empty());
    connection.actionError.clear();

    connection.sendUiEvent("reopened", "spotify-next", "press", nullptr, 2);
    reply(connection.nextRequest(), false, "");
    assert(connection.actionError == "widget-rejected");
    connection.resyncWidgetSession("reopened");
    assert(connection.nextRequest().at("type") == "AttachUiSession");

    connection.actionError.clear();
    connection.sendUiEvent("reopened", "volume", "adjust", 0.75, 2);
    reply(connection.nextRequest(), false, "");
    assert(connection.actionError.empty() && connection.outgoingQueue_.empty());
    assert(!connection.handleUiSessionMessage(tree));

    connection.sendUiEvent("reopened", "volume", "adjust", 0.75, 2);
    reply(connection.nextRequest(), false, "");
    assert(connection.actionError == "widget-rejected");
    assert(connection.nextRequest().at("type") == "AttachUiSession");
    assert(connection.outgoingQueue_.empty());
    connection.expire("widget");
    connection.syncWidgetSessions();
    assert(connection.nextRequest().at("type") == "CloseUiSession");
    assert(!connection.hasWidgetSession("widget"));
    connection.expire("widget");
    connection.syncWidgetSessions();
    reply(connection.nextRequest(), true, "latest");
    assert(connection.nextRequest().at("type") == "AttachUiSession");
    assert(!connection.handleUiSessionMessage({{"kind", "message"}, {"type", "UiSessionClosedEvent"},
        {"payload", {{"sessionId", "latest"}}}}));
    assert(!connection.hasWidgetSession("widget"));
    connection.expire("widget");
    connection.syncWidgetSessions();
    assert(connection.nextRequest().at("type") == "OpenWidgetUiSession");
}

std::string widgetArtworkId(const nlohmann::json& props) {
    const auto source = props.find("source");
    if (source != props.end() && source->is_object()) {
        return jsonStr(*source, "resourceId", jsonStr(*source, "reference", jsonStr(*source, "id")));
    }
    return jsonStr(props, "source", jsonStr(props, "resourceId", jsonStr(props, "reference")));
}

std::string widgetArtworkKey(const nlohmann::json& props) {
    auto id = widgetArtworkId(props);
    const auto source = props.find("source");
    if (id.empty() || source == props.end() || !source->is_object()) return id;
    const auto hash = jsonStr(*source, "contentHash");
    if (!hash.empty()) id += '\n' + hash;
    return id;
}


struct ArtworkTest {
    struct Transition {
        std::string shownId, previousId;
        Json shownStyle, previousStyle;
        Uint64 started = 0, lastSeen = 0;
    };
    struct Renderer {
        std::map<std::pair<std::string, std::string>, Transition> artworkTransitions_;
    } ui;
    struct TextureCache {
        std::set<std::string> ready;
        const void* getResourceTexture(const std::string& id) const { return ready.count(id) ? this : nullptr; }
    } state;
    DeckButton button;
    Uint64 now = 1000;
    std::vector<std::string> drawnIds;
    void artwork(const Json&, const Json&, SDL_Rect, double);
    void image(const std::string& id, const Json&, SDL_Rect, double, double = 1) {
        if (!id.empty()) drawnIds.push_back(id);
    }
};

    void ArtworkTest::artwork(const Json& node, const Json& props, SDL_Rect area, double inheritedOpacity) {
        const auto styleOpacity = [&](const Json& style) {
            return inheritedOpacity * std::clamp(jsonDouble(style, "opacity", 1), 0.0, 1.0);
        };
        const double opacity = styleOpacity(props);
        const auto source = widgetArtworkId(props);
        auto id = source == button.musicArtworkId && !button.musicArtworkKey.empty()
            ? button.musicArtworkKey : widgetArtworkKey(props);
        if (id.empty()) {
            if (jsonStr(node, "type") == "ui.button") image(id, props, area, opacity);
            return;
        }
        const bool cover = button.widgetType == "MusicPlayer" && !source.empty() && source == button.musicArtworkId;
        if ((!cover && jsonStr(props, "transition") != "crossfade") || jsonStr(node, "id").empty()) {
            image(id, props, area, opacity);
            return;
        }
        auto& animation = ui.artworkTransitions_[{button.widgetId, jsonStr(node, "id")}];
        const auto* texture = state.getResourceTexture(id);
        if (cover && id != animation.shownId && !texture) animation = {};
        animation.lastSeen = now;
        if (id != animation.shownId && texture) {
            animation.previousId = std::move(animation.shownId);
            animation.previousStyle = std::move(animation.shownStyle);
            animation.shownId = id;
            animation.started = now;
        }
        if (id == animation.shownId && animation.shownStyle != props) animation.shownStyle = props;
        const double phase = std::min(1.0, (now - animation.started) / 220.0);
        const double eased = 1.0 - std::pow(1.0 - phase, 3);
        if (!animation.previousId.empty() && phase < 1 &&
            state.getResourceTexture(animation.previousId)) {
            image(animation.previousId, animation.previousStyle, area, styleOpacity(animation.previousStyle));
            image(animation.shownId, animation.shownStyle, area, opacity * eased, 1 + 0.02 * (1 - eased));
        } else {
            image(animation.shownId, animation.shownStyle, area, styleOpacity(animation.shownStyle));
            if (phase >= 1) animation.previousId.clear();
        }
    }

void testMusicArtwork() {
    ArtworkTest cover;
    cover.button.widgetType = "MusicPlayer";
    cover.button.widgetId = "music";
    cover.button.musicArtworkId = "cover";
    const Json node = {{"id", "album-cover"}, {"type", "ui.image"}};
    const Json props = {{"source", {{"resourceId", "cover"}}}};
    cover.button.musicArtworkKey = "first";
    cover.state.ready = {"first"};
    cover.artwork(node, props, {0, 0, 100, 100}, 1);
    assert(cover.drawnIds == std::vector<std::string>{"first"});

    cover.button.musicArtworkKey = "second";
    cover.drawnIds.clear();
    cover.artwork(node, props, {0, 0, 100, 100}, 1);
    assert(cover.drawnIds.empty());
    cover.state.ready.insert("second");
    cover.artwork(node, props, {0, 0, 100, 100}, 1);
    assert(cover.drawnIds == std::vector<std::string>{"second"});
}

void testConfigRecovery() {
    namespace fs = std::filesystem;
    const auto originalDirectory = fs::current_path();
    const auto testDirectory = fs::temp_directory_path() / ("switchdeck-config-test-" + std::to_string(std::random_device{}()));
    assert(fs::create_directory(testDirectory));
    fs::current_path(testDirectory);
    AppConfig config;
    config.client_id = "test-client";
    config.refresh_cookie = "test-cookie";
    config.show_status_bar = false;
    ConfigManager::save(config);
    assert(!ConfigManager::load().show_status_bar);

    fs::rename("config.ini", "config.ini.bak");
    std::ofstream("config.ini.tmp") << "partial write";
    assert(ConfigManager::load().refresh_cookie == "test-cookie");
    config.theme = 2;
    ConfigManager::save(config);
    const auto recovered = ConfigManager::load();
    assert(recovered.theme == 2 && recovered.refresh_cookie == "test-cookie");
    assert(!fs::exists("config.ini.bak") && !fs::exists("config.ini.tmp"));

    assert(fs::create_directory("config.ini.tmp"));
    config.refresh_cookie.clear();
    ConfigManager::save(config);
    assert(ConfigManager::load().refresh_cookie == "test-cookie");
    fs::current_path(originalDirectory);
    fs::remove_all(testDirectory);
}


struct SessionState {
std::vector<DeckFolder> folders_;std::vector<DeckButton> buttons_;std::string currentFolderId_="folder";int invalidated=0;
std::map<std::string,std::string> sessionToWidget_;
DeckButton* getButtonById(const std::string& id){for(auto& b:buttons_)if(b.widgetId==id)return &b;return nullptr;}
DeckButton parseMd3Widget(const Json& j,const std::string& folder){DeckButton b;b.widgetId=jsonStr(j,"id");b.folderId=folder;b.widgetType=jsonStr(j,"type");b.clockConfiguredAnalog=jsonStr(j,"style")=="analog";b.clockIsAnalog=b.clockConfiguredAnalog;return b;}
void handleWidgetStateInvalidated(const std::string&){++invalidated;}
void registerWidgetSession(const std::string&,const std::string&);
void handleUiSessionClosedOrInvalidated(const std::string&);
void extractStateFromUiTree(DeckButton&){}
void syncActiveFolderToButtons(SDL_Renderer*){buttons_=folders_[0].widgets;}
bool handleMd3WidgetUpsert(const std::string&,const Json&,SDL_Renderer*);
const DeckButton* findSpotifyPlayAction(const DeckButton&) const;
};
void DeckButton::cleanupTextures(){}

static void pauseInvalidatedMusic(DeckButton& button) {
    if (button.widgetType != "MusicPlayer") return;
    button.progressPositionMs = button.getCurrentProgressMs(SDL_GetTicks64(), button.progressDurationMs > 0);
    button.progressRate = 0;
    button.progressAnchorTicks = 0;
    button.progressAnchorOffsetMs = 0;
    button.musicIsPlaying = false;
}

bool SessionState::handleMd3WidgetUpsert(const std::string& folderId, const nlohmann::json& widgetJson, SDL_Renderer* renderer) {
    if (!widgetJson.is_object()) return false;
    std::string wid = jsonStr(widgetJson, "id", "");
    if (wid.empty()) return false;
    bool reopen = false;

    // Also check if the widget is currently active in buttons_ so we don't lose its live uiTree/sessionId
    DeckButton* liveBtn = getButtonById(wid);

    std::string targetFolderId = folderId.empty() ? currentFolderId_ : folderId;
    for (auto& f : folders_) {
        if (f.id == targetFolderId) {
            DeckButton newBtn = parseMd3Widget(widgetJson, f.id);
            bool found = false;
            for (auto& w : f.widgets) {
                if (w.widgetId == wid) {
                    const DeckButton& src = (liveBtn && !liveBtn->uiTree.is_null()) ? *liveBtn : w;
                    const bool clockModeChanged = newBtn.widgetType == "Clock" && w.widgetType == "Clock" &&
                                                  w.clockConfiguredAnalog != newBtn.clockConfiguredAnalog;
                    const bool sliderValueVisibilityChanged = newBtn.widgetType == "Slider" && w.widgetType == "Slider" &&
                        w.sliderShowValue != newBtn.sliderShowValue;
                    if (clockModeChanged || sliderValueVisibilityChanged) {
                        if (liveBtn && liveBtn->folderId == f.id) {
                            liveBtn->uiTree = nullptr;
                            handleWidgetStateInvalidated(wid);
                            reopen = true;
                        }
                    } else {
                        copyRuntimeState(src, newBtn);
                    }
                    w.cleanupTextures();
                    w = std::move(newBtn);
                    if (!w.uiTree.is_null()) extractStateFromUiTree(w);
                    found = true;
                    break;
                }
            }
            if (!found) {
                f.widgets.push_back(std::move(newBtn));
            }
            break;
        }
    }

    if (targetFolderId == currentFolderId_) {
        syncActiveFolderToButtons(renderer);
    }
    return reopen;
}

const DeckButton* SessionState::findSpotifyPlayAction(const DeckButton& player) const {
    if (player.widgetType != "MusicPlayer" || player.musicHasTrack) return nullptr;
    std::string source = player.musicInstanceId + " " + player.musicProviderName + " " + player.musicSourceName;
    std::transform(source.begin(), source.end(), source.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool spotifySource = source.find("spotify") != std::string::npos || source.find("spicetify") != std::string::npos;
    if (!player.musicInstanceId.empty() && !spotifySource) return nullptr;
    if (player.spotifyPlayAction) return &player;
    for (const auto& button : buttons_) {
        if (button.folderId == player.folderId && button.spotifyPlayAction) return &button;
    }
    return nullptr;
}

void SessionState::registerWidgetSession(const std::string& widgetId, const std::string& sessionId) {
    if (widgetId.empty() || sessionId.empty()) return;
    for (auto it = sessionToWidget_.begin(); it != sessionToWidget_.end();) {
        if (it->second == widgetId && it->first != sessionId) it = sessionToWidget_.erase(it);
        else ++it;
    }
    sessionToWidget_[sessionId] = widgetId;
    const auto registerSession = [&](DeckButton& button) {
        if (button.sessionId == sessionId) return;
        button.sessionId = sessionId;
        button.revision = 0;
        button.primaryNodeId.clear();
        button.primaryNodeEvents.clear();
    };
    if (DeckButton* btn = getButtonById(widgetId)) {
        registerSession(*btn);
    }
    for (auto& f : folders_) {
        for (auto& w : f.widgets) {
            if (w.widgetId == widgetId) {
                registerSession(w);
            }
        }
    }
}

void SessionState::handleUiSessionClosedOrInvalidated(const std::string& sessionId) {
    if (sessionId.empty()) return;
    auto it = sessionToWidget_.find(sessionId);
    if (it != sessionToWidget_.end()) {
        std::string wid = it->second;
        sessionToWidget_.erase(it);
        if (DeckButton* btn = getButtonById(wid); btn && btn->sessionId == sessionId) {
            btn->sessionId.clear();
            btn->revision = 0;
            btn->primaryNodeId.clear();
            btn->primaryNodeEvents.clear();
            pauseInvalidatedMusic(*btn);
        }
        for (auto& f : folders_) {
            for (auto& w : f.widgets) {
                if (w.widgetId == wid && w.sessionId == sessionId) {
                    w.sessionId.clear();
                    w.revision = 0;
                    w.primaryNodeId.clear();
                    w.primaryNodeEvents.clear();
                    pauseInvalidatedMusic(w);
                }
            }
        }
    }
}

const Json& object(const Json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_object() ? *it : emptyObject;
}

const Json& children(const Json& value) {
    const auto it = value.find("children");
    return it != value.end() && it->is_array() ? *it : emptyArray;
}

int alignedOffset(const std::string& align, int free) {
    if (align == "center") return free / 2;
    return align == "end" || align == "right" || align == "bottom" ? free : 0;
}

SDL_Rect inset(SDL_Rect box, int padding) {
    const int x = std::min(box.w / 2, padding);
    const int y = std::min(box.h / 2, padding);
    return {box.x + x, box.y + y, box.w - x * 2, box.h - y * 2};
}

bool textComponent(const std::string& type) {
    return type == "ui.text" || type == "macrodeck.progress-text" || type == "macrodeck.dynamic-text";
}

bool levelComponent(const std::string& type) {
    return type == "ui.slider" || type == "macrodeck.slider" || type == "ui.dial";
}

struct Layout {
    struct Box {
        const Json* node = nullptr;
        SDL_Rect area{};
        std::vector<Box> child;
        bool supported = true;
        bool textFits = true;
        int parentCross = 0;
    };

    FakeUi& ui;
    const DeckButton& button;
    const int& state;
    const AppConfig& config;
    const int basis;
    const double cellScale;
    const Uint64 now;
    int tileRadius = 0;
    int remainingNodes = 1200;

    int length(const Json& props, const char* key, SDL_Rect parent, int fallback = 0, int cross = 0) const {
        const auto it = props.find(key);
        if (it == props.end()) return fallback;
        double pixels = fallback;
        if (it->is_number()) {
            pixels = it->get<double>() * basis;
        } else if (it->is_object()) {
            pixels = jsonDouble(*it, "basis", fallback / static_cast<double>(basis)) * basis;
            const double parentFraction = jsonDouble(*it, "ofParent", -1);
            if (parentFraction >= 0) pixels = parentFraction * std::min(parent.w, parent.h);
            const double maxCross = jsonDouble(*it, "maxOfCross", -1);
            const double maxCell = jsonDouble(*it, "maxOfCell", -1);
            if (maxCross >= 0 && cross > 0) pixels = std::min(pixels, maxCross * cross);
            if (maxCell >= 0) pixels = std::min(pixels, maxCell * 120 * cellScale);
        }
        return std::isfinite(pixels) ? static_cast<int>(std::lround(std::clamp(pixels, 0.0, 2560.0))) : fallback;
    }

    std::string label(const Json& node, const Json& props, const std::string& type) const {
        if (type == "macrodeck.dynamic-text") {
            const auto& culture = widgetCulture();
            const int language = culture.rfind("tr", 0) == 0 ? 0 : culture.rfind("en", 0) == 0 ? 1 : config.language;
            return dynamicLabel(props, button, language);
        }
        const auto id = jsonStr(node, "id");
        const auto key = id.substr(id.find_last_of("./:") + 1);
        if (button.widgetType == "Countdown" && button.countdownIsIdle() &&
            (type == "macrodeck.progress-text" || key == "time" || key == "timeFallback" ||
             key == "remaining" || key == "readout" || key == "countdownTime")) {
            return button.countdownReadout(now);
        }
        if (button.widgetType == "Slider" &&
            (key == "value" || key == "lead" || key == "valueText" || key == "percent")) {
            if (!button.sliderShowValue) return {};
            if (button.sliderValueText.empty() || button.sliderValueText.find('%') != std::string::npos ||
                (button.sliderMin == 0 && (button.sliderMax == 1 || button.sliderMax == 100))) {
                return std::to_string(static_cast<int>(std::lround(std::clamp(button.sliderLevel, 0.0f, 1.0f) * 100))) + "%";
            }
        }
        if (type == "macrodeck.progress-text") {
            if (button.widgetType == "MusicPlayer" && !button.musicHasTrack) return {};
            const auto timer = button.widgetType == "Countdown" || button.widgetType == "Stopwatch";
            if (timer && !button.treeHasProgress) {
                const auto text = widgetNodeText(node, props);
                if (!text.empty()) return text;
            }
            auto format = jsonStr(props, "format", "elapsed");
            if (format == "duration" && button.progressDurationMs > 0) {
                return DeckButton::formatDurationMs(button.progressDurationMs);
            }
            if (format == "remaining" && button.progressDurationMs > 0) {
                if (button.widgetType == "Countdown") return button.countdownReadout(now);
                return DeckButton::formatDurationMs(button.progressDurationMs - button.getCurrentProgressMs(now));
            }
            if (format == "elapsed") {
                const bool bounded = button.widgetType == "MusicPlayer" && button.progressDurationMs > 0;
                return DeckButton::formatDurationMs(button.getCurrentProgressMs(now, bounded));
            }
            return {};
        }
        return widgetNodeText(node, props);
    }

    bool bold(const Json& props) const {
        const auto weight = jsonStr(props, "weight");
        return weight == "bold" || weight == "semibold" || weight == "semi-bold" || jsonInt(props, "weight") >= 600;
    }

    int fontSize(const Json& props, SDL_Rect box, int parentCross = 0) const {
        return std::clamp(length(props, "size", box, std::max(8, basis / 8), parentCross), 4, 160);
    }

    const Json* responsiveChild(const Json& node, const SDL_Rect& box) const {
        const double cellPixels = 120 * cellScale;
        return selectResponsiveChild(node, box.w / cellPixels, box.h / cellPixels);
    }

    SDL_Point measure(const Json& node, SDL_Rect bounds, int depth = 0, int parentCross = 0) {
        if (depth > 25 || --remainingNodes < 0 || !node.is_object()) return {};
        const auto type = jsonStr(node, "type");
        const auto& props = object(node, "properties");
        if (type == "ui.responsive") {
            const auto* choice = responsiveChild(node, bounds);
            return choice ? measure(*choice, bounds, depth + 1, parentCross) : SDL_Point{};
        }
        const auto& items = children(node);
        if (type == "ui.first-fit" && !items.empty()) return measure(items.back(), bounds, depth + 1, parentCross);
        if (textComponent(type)) {
            const int size = fontSize(props, bounds, parentCross);
            auto* font = ui.widgetFont(size, bold(props), jsonStr(props, "fontFace"));
            const auto text = label(node, props, type);
            const int line = font ? TTF_FontLineSkip(font) : size;
            const int width = ui.getTextWidth(font, text);
            const int wrappedRows = jsonBool(props, "wrap") ?
                std::clamp((width + std::max(1, bounds.w) - 1) / std::max(1, bounds.w), 1,
                           std::clamp(jsonInt(props, "maxLines", 4), 1, 12)) : 1;
            return {jsonBool(props, "wrap") ? std::min(width, bounds.w) : width, text.empty() ? 0 : line * wrappedRows};
        }
        if (type == "ui.icon" || type == "ui.image") {
            const int size = length(props, "size", bounds, basis / 4);
            return {size, size};
        }
        if (type == "ui.range-bar" || type == "macrodeck.progress-bar" || (levelComponent(type) && type != "ui.dial")) {
            const int thickness = length(props, "thickness", bounds, std::max(2, basis / 22));
            return jsonStr(props, "direction") == "vertical" ? SDL_Point{thickness, 0} : SDL_Point{0, thickness};
        }
        if (type == "ui.gauge" || type == "ui.dial" || type == "macrodeck.clock-dial" || type == "ui.chart") {
            const int natural = type == "macrodeck.clock-dial" ? basis * 4 / 5 :
                                (type == "ui.gauge" || type == "ui.dial" ? basis / 2 : 0);
            return {natural, natural};
        }
        if (type == "ui.grid") {
            const int min = length(props, "minCellSize", bounds);
            const int columns = gridColumns(props, bounds, min);
            const int statedRows = jsonInt(props, "rows");
            const int rows = min == 0 && statedRows > 0 ? std::clamp(statedRows, 1, 32) :
                             std::max(1, (static_cast<int>(items.size()) + columns - 1) / columns);
            const int gap = length(props, "gap", bounds);
            int rowHeight = min;
            int columnWidth = min;
            for (const auto& item : items) {
                const auto size = measure(item, bounds, depth + 1);
                columnWidth = std::max(columnWidth, size.x);
                rowHeight = std::max(rowHeight, size.y);
            }
            const int padding = length(props, "padding", bounds);
            return {std::min(2560, columns * columnWidth + (columns - 1) * gap + 2 * padding),
                    std::min(2560, rows * rowHeight + (rows - 1) * gap + 2 * padding)};
        }
        const bool stack = type == "ui.stack" || type == "ui.button" || type == "ui.list";
        const bool across = jsonStr(props, "direction") == "horizontal";
        const int padding = length(props, "padding", bounds);
        const int gap = length(props, "gap", bounds);
        const auto inner = inset(bounds, padding);
        SDL_Point size{};
        for (const auto& item : items) {
            auto child = measure(item, inner, depth + 1, stack ? (across ? inner.h : inner.w) : 0);
            const auto& style = object(item, "properties");
            if (across) child.x = length(style, "mainSize", inner, child.x, inner.h);
            else child.y = length(style, "mainSize", inner, child.y, inner.w);
            size.x = stack && across ? size.x + child.x : std::max(size.x, child.x);
            size.y = stack && !across ? size.y + child.y : std::max(size.y, child.y);
        }
        if (stack && !items.empty()) {
            (across ? size.x : size.y) += gap * static_cast<int>(items.size() - 1);
        }
        if (type == "ui.button" && items.empty()) size = {basis / 4, basis / 4};
        return {std::min(size.x + padding * 2, 2560), std::min(size.y + padding * 2, 2560)};
    }

    Box layout(const Json& node, SDL_Rect area, int depth = 0, int parentCross = 0) {
        Box box{&node, area, {}, true, true, parentCross};
        if (depth > 28 || --remainingNodes < 0 || !node.is_object()) {
            box.supported = false;
            return box;
        }
        const auto type = jsonStr(node, "type");
        const auto& props = object(node, "properties");
        const auto& items = children(node);
        if (type == "ui.responsive") {
            const auto* choice = responsiveChild(node, area);
            if (choice) return layout(*choice, area, depth + 1, parentCross);
            box.supported = false;
            return box;
        }
        if (type == "ui.first-fit") {
            for (const auto& item : items) {
                box = layout(item, area, depth + 1, parentCross);
                if (box.supported && box.textFits) return box;
            }
            if (items.empty()) box.supported = false;
            return box;
        }
        const bool stack = type == "ui.stack" || type == "ui.button" || type == "ui.list";
        const bool layer = type == "ui.layer";
        const bool modifier = type == "ui.modifier" && !props.contains("mask");
        const bool transform = type == "ui.transform" &&
                               jsonDouble(props, "rotation", jsonDouble(props, "rotate", 0)) == 0;
        const bool text = textComponent(type);
        const bool grid = type == "ui.grid";
        const bool leaf = text || type == "ui.image" || type == "ui.icon" ||
                          type == "ui.range-bar" || type == "macrodeck.progress-bar" || type == "ui.spacer" ||
                          type == "ui.shape" || type == "ui.gauge" || type == "ui.chart" ||
                          type == "macrodeck.clock-dial" || levelComponent(type);
        const int maxVersion = type == "macrodeck.dynamic-text" || type == "ui.stack" || type == "ui.button" || type == "ui.icon" ? 2 : 1;
        const bool version = jsonInt(node, "requiredComponentVersion", 1) <= maxVersion;
        if (!version || (!stack && !layer && !modifier && !transform && !grid && !leaf)) {
            if (node.contains("fallback")) return layout(node["fallback"], area, depth + 1, parentCross);
            box.supported = false;
            return box;
        }
        if (text) {
            const int size = fontSize(props, area, parentCross);
            const int smallest = std::clamp(length(props, "minSize", area, size, parentCross), 4, size);
            auto* font = ui.widgetFont(smallest, bold(props), jsonStr(props, "fontFace"));
            box.textFits = ui.getTextWidth(font, label(node, props, type)) <= area.w && (!font || TTF_FontHeight(font) <= area.h + 1);
        }
        if (leaf || items.empty()) return box;
        if (grid) return layoutGrid(node, area, depth);

        if (modifier) {
            const auto& frame = object(props, "frame");
            const int width = std::min(area.w, length(frame, "width", area,
                             length(frame, "maxWidth", area, area.w)));
            const int height = std::min(area.h, length(frame, "height", area,
                              length(frame, "maxHeight", area, area.h)));
            const double ratio = jsonDouble(frame, "aspectRatio");
            const int fittedW = ratio > 0 ? std::min(width, static_cast<int>(std::min(width * 1.0, height * ratio))) : width;
            const int fittedH = ratio > 0 ? std::min(height, static_cast<int>(std::min(height * 1.0, fittedW / ratio))) : height;
            area = {area.x + (area.w - fittedW) / 2, area.y + (area.h - fittedH) / 2, fittedW, fittedH};
            box.area = area;
        }
        if (transform) {
            const auto zoom = jsonDouble(props, "zoom", jsonDouble(props, "scale", 1));
            const double scale = zoom <= 0 ? 1 : std::clamp(zoom, 0.1, 4.0);
            const int w = static_cast<int>(std::lround(std::min(4096.0, area.w * scale)));
            const int h = static_cast<int>(std::lround(std::min(4096.0, area.h * scale)));
            const double pivotX = std::clamp(jsonDouble(props, "originX", 0.5), -2.0, 3.0);
            const double pivotY = std::clamp(jsonDouble(props, "originY", 0.5), -2.0, 3.0);
            const double offsetX = std::clamp(jsonDouble(props, "offsetX"), -4.0, 4.0);
            const double offsetY = std::clamp(jsonDouble(props, "offsetY"), -4.0, 4.0);
            const int x = area.x + static_cast<int>(std::lround((area.w - w) * pivotX + area.w * offsetX));
            const int y = area.y + static_cast<int>(std::lround((area.h - h) * pivotY + area.h * offsetY));
            area = {x, y, w, h};
        }
        const SDL_Rect inner = inset(area, length(props, "padding", area));
        const bool across = jsonStr(props, "direction") == "horizontal";
        const int span = across ? inner.w : inner.h;
        const int cross = across ? inner.h : inner.w;
        int gap = length(props, "gap", inner);
        if (items.size() > 1) gap = std::min(gap, span / static_cast<int>(items.size() - 1));
        const int available = span - gap * static_cast<int>(items.size() - 1);
        const bool list = type == "ui.list";
        const bool clipStart = jsonStr(props, "overflow") == "clip-start" || (list && jsonStr(props, "anchor") == "end");
        std::vector<int> extents;
        std::vector<int> crossExtents;
        int fixed = 0;
        int fills = 0;
        if (stack) {
            for (const auto& item : items) {
                const auto& style = object(item, "properties");
                const auto natural = measure(item, inner, depth + 1, cross);
                const auto childType = jsonStr(item, "type");
                const bool autoFill = !style.contains("mainSize") && !style.contains("fill") &&
                    (childType == "macrodeck.clock-dial" || (button.widgetType == "Gauges" && childType == "ui.grid"));
                const bool fill = !list && !clipStart &&
                    (autoFill || (jsonBool(style, "fill") && !style.contains("mainSize")));
                const int contentExtent = across ? natural.x : natural.y;
                const int extent = list ? contentExtent : length(style, "mainSize", inner, contentExtent, cross);
                extents.push_back(fill ? -1 : extent);
                crossExtents.push_back(across ? natural.y : natural.x);
                if (fill) ++fills;
                else fixed += extent;
            }
        }
        if (stack && fixed > available) box.textFits = false;
        const auto justify = jsonStr(props, "justify", "start");
        const auto align = jsonStr(props, "align", "stretch");
        const int leftover = std::max(0, available - fixed);
        int offset = clipStart && fixed > available ? available - fixed :
                     (fills == 0 ? alignedOffset(justify, leftover) : 0);
        if (fills == 0 && justify == "space-between" && items.size() > 1) {
            gap += leftover / static_cast<int>(items.size() - 1);
        }
        int consumed = 0;
        int shrinkSum = 0;
        size_t index = 0;
        for (const auto& item : items) {
            SDL_Rect position = inner;
            if (stack) {
                int extent = extents[index];
                if (extent < 0) {
                    extent = fills > 0 ? leftover / fills : 0;
                } else if (!list && !clipStart && fixed > available) {
                    shrinkSum += extent;
                    const int scaledEnd = static_cast<int>((static_cast<int64_t>(shrinkSum) * available) / fixed);
                    extent = scaledEnd - consumed;
                    consumed = scaledEnd;
                }
                const bool childFills = jsonBool(object(item, "properties"), "fill");
                const int width = align == "stretch" || childFills ? cross : std::min(cross, crossExtents[index]);
                const int insetCross = alignedOffset(across && align == "baseline" ? "end" : align, cross - width);
                position = across ? SDL_Rect{inner.x + offset, inner.y + insetCross, extent, width}
                                  : SDL_Rect{inner.x + insetCross, inner.y + offset, width, extent};
                offset += extent + gap;
            }
            auto sub = layout(item, position, depth + 1, stack ? cross : 0);
            box.supported = box.supported && sub.supported;
            box.textFits = box.textFits && sub.textFits;
            box.child.push_back(std::move(sub));
            ++index;
        }
        return box;
    }

    int gridColumns(const Json& props, SDL_Rect inner, int minCell) const {
        if (minCell <= 0) return std::clamp(jsonInt(props, "columns", 1), 1, 32);
        const int gap = length(props, "gap", inner);
        return std::clamp((inner.w + gap) / (minCell + gap), 1, 32);
    }

    Box layoutGrid(const Json& node, SDL_Rect area, int depth) {
        Box result{&node, area, {}, true, true};
        const auto& props = object(node, "properties");
        const auto inner = inset(area, length(props, "padding", area));
        const int minCell = length(props, "minCellSize", inner);
        const int columns = gridColumns(props, inner, minCell);
        const int fixedRows = minCell > 0 ? 0 : std::clamp(jsonInt(props, "rows"), 0, 32);
        const int limit = fixedRows > 0 ? fixedRows : 32;
        struct Slot { const Json* item; int x, y, w, h; };
        std::vector<Slot> placed;
        std::array<uint32_t, 32> occupied{};
        int usedRows = 0;
        for (const auto& item : children(node)) {
            const auto& style = object(item, "properties");
            const int width = std::clamp(jsonInt(style, "columnSpan", 1), 1, columns);
            const int height = std::clamp(jsonInt(style, "rowSpan", 1), 1, limit);
            bool found = false;
            for (int y = 0; y + height <= limit && !found; ++y) {
                for (int x = 0; x + width <= columns && !found; ++x) {
                    const uint32_t bits = static_cast<uint32_t>(((uint64_t{1} << width) - 1) << x);
                    if (std::any_of(occupied.begin() + y, occupied.begin() + y + height,
                                    [&](uint32_t row) { return (row & bits) != 0; })) continue;
                    for (int row = y; row < y + height; ++row) occupied[row] |= bits;
                    placed.push_back({&item, x, y, width, height});
                    usedRows = std::max(usedRows, y + height);
                    found = true;
                }
            }
            if (!found) result.textFits = false;
        }
        const int rows = std::max(1, fixedRows > 0 ? fixedRows : usedRows);
        int gap = length(props, "gap", inner);
        if (columns > 1) gap = std::min(gap, inner.w / (columns - 1));
        if (rows > 1) gap = std::min(gap, inner.h / (rows - 1));
        const auto start = [gap](int index, int total, int count) {
            return index * (total - (count - 1) * gap) / count + index * gap;
        };
        for (const auto& slot : placed) {
            const int x = start(slot.x, inner.w, columns);
            const int y = start(slot.y, inner.h, rows);
            const int width = start(slot.x + slot.w, inner.w, columns) - x - gap;
            const int height = start(slot.y + slot.h, inner.h, rows) - y - gap;
            auto child = layout(*slot.item, {inner.x + x, inner.y + y, width, height}, depth + 1);
            result.supported = result.supported && child.supported;
            result.textFits = result.textFits && child.textFits;
            result.child.push_back(std::move(child));
        }
        return result;
    }

};


Json read(const char* path){std::ifstream f(path);assert(f);Json j;f>>j;return j;}
const Json* find(const Json& n,const std::string& leaf){if(jsonStr(n,"id")==leaf)return &n;for(const auto& c:children(n))if(auto p=find(c,leaf))return p;return nullptr;}
const Layout::Box* findBox(const Layout::Box& b,const std::string& id){if(b.node&&jsonStr(*b.node,"id")==id)return &b;for(const auto& c:b.child)if(auto p=findBox(c,id))return p;return nullptr;}
int main(){
 testWidgetSessionRecovery();
 testMusicArtwork();
 testConfigRecovery();
 assert(*parseReleaseVersion("v1.0.1") > *parseReleaseVersion("v1.0.0"));
 assert(*parseReleaseVersion("v1.10.0") > *parseReleaseVersion("v1.9.9"));
 assert(parseReleaseVersion("1.0.1")==parseReleaseVersion("v1.0.1"));
 assert(!parseReleaseVersion("v1.0") && !parseReleaseVersion("v1.0.1-rc1"));
 ConnectionTest connection;
 connection.pong(0,2500);assert(connection.pingMs_==-1 && connection.awaitingPong_);
 connection.pong(1,10);assert(connection.pingMs_>=10 && !connection.awaitingPong_);
 connection.pong(1,2500);assert(connection.pingMs_<100);
 connection.pingSequence_=2;connection.awaitingPong_=true;
 connection.pong(2,1800);assert(connection.pingMs_<100);
 connection.pingSequence_=3;connection.awaitingPong_=true;
 connection.pong(3,1800);assert(connection.pingMs_>=1800);
 setWidgetLocalization(read("tests/fixtures/catalog.json"));
 assert(widgetCulture()=="tr-TR");
 SessionState sessions;DeckFolder folder;folder.id="folder";
 DeckButton clock;clock.widgetId="clock";clock.folderId="folder";clock.widgetType="Clock";clock.clockConfiguredAnalog=true;clock.clockIsAnalog=true;clock.sessionId="old";clock.uiTree={{"id","old-dial"}};
 folder.widgets.push_back(clock);sessions.folders_.push_back(folder);sessions.buttons_.push_back(clock);
 assert(sessions.handleMd3WidgetUpsert("folder",{{"id","clock"},{"type","Clock"},{"style","digital"}},nullptr));
 assert(sessions.invalidated==1);assert(!sessions.buttons_[0].clockIsAnalog);assert(sessions.buttons_[0].uiTree.is_null());assert(sessions.buttons_[0].sessionId.empty());
 sessions.buttons_[0].sessionId="new";sessions.buttons_[0].uiTree={{"id","digital"}};
 assert(!sessions.handleMd3WidgetUpsert("folder",{{"id","clock"},{"type","Clock"},{"style","digital"}},nullptr));
 assert(sessions.buttons_[0].sessionId=="new");
 sessions.registerWidgetSession("clock","new");sessions.buttons_[0].revision=12;
 sessions.registerWidgetSession("clock","new");assert(sessions.buttons_[0].revision==12);
 sessions.registerWidgetSession("clock","reopened");
 sessions.handleUiSessionClosedOrInvalidated("new");
 assert(sessions.buttons_[0].sessionId=="reopened" && sessions.buttons_[0].revision==0);
 DeckButton player;player.widgetType="MusicPlayer";player.musicProviderName="Spotify";player.spotifyPlayAction=true;
 assert(sessions.findSpotifyPlayAction(player)==&player);player.musicHasTrack=true;assert(!sessions.findSpotifyPlayAction(player));
 player.musicHasTrack=false;player.musicInstanceId="other";player.musicProviderName="Other";assert(!sessions.findSpotifyPlayAction(player));
 const auto weather=read("tests/fixtures/Weather-tree.json")["tree"]["root"];
 DeckButton w;fillWeatherDetails(weather,w,false);
 assert(w.weatherTemp=="15°");assert(!w.weatherCondition.empty());assert(w.weatherHighLow=="Y: 17° D: 14°");
 const auto slider=read("tests/fixtures/Slider-tree.json")["tree"]["root"];
 const auto* value=find(slider,"slider.value");assert(value);assert(widgetNodeText(*value,(*value)["properties"])=="100 %");
 assert(localizedWidgetText({{"scope","missing"},{"key","missing"}}).empty());
 setWidgetLocalization({{"culture","en"},{"translations",{{"test:braces","{a}/{b}"}}}});
 assert(localizedWidgetText({{"scope","test"},{"key","braces"},{"arguments",{{"a","{b}"},{"b",5}}}})=="{b}/5");
 setWidgetLocalization(read("tests/fixtures/catalog.json"));
 DeckButton timer;timer.countdownConfiguredMs=600000;timer.progressDurationMs=600999;timer.progressRate=0;
 assert(timer.countdownIsIdle());assert(timer.countdownReadout(1000)=="10:00");
 timer.countdownIsRunning=true;timer.progressRate=1;timer.progressAnchorTicks=1000;
 assert(timer.countdownReadout(1000)=="10:00");assert(timer.countdownReadout(1999)=="10:00");
 assert(timer.countdownReadout(2000)=="9:59");
 timer.progressPositionMs=2000;
 assert(timer.countdownReadout(2000)=="9:57");
 timer.countdownIsRunning=false;timer.countdownIsPaused=true;timer.progressRate=0;
 assert(!timer.countdownIsIdle());assert(timer.countdownReadout(5000)=="9:58");
 DeckButton stopwatch;stopwatch.progressRate=0;stopwatch.progressPositionMs=0;assert(stopwatch.getCurrentProgressMs(900000,false)==0);
 stopwatch.progressAnchorTicks=1000;stopwatch.progressRate=1;assert(stopwatch.getCurrentProgressMs(1123,false)==123);
 std::tm date{};date.tm_year=126;date.tm_mon=10;date.tm_mday=21;date.tm_wday=6;
 assert(formatClockDate(date,"date-long",0)=="21 Kasım Cumartesi");assert(formatClockDate(date,"date-iso",0)=="2026-11-21");
 FakeUi fake;AppConfig cfg;
 for(const auto& icon:{"device-desktop","device-laptop","device-phone","device-tablet","device-watch","device-headphones","device-gamepad","device-mouse","device-keyboard","device-speaker","battery","battery-charging"}){int before=iconDraws;fake.drawVectorIcon(icon,30,30,24,{255,255,255,255});assert(iconDraws>before);}
 fake.drawText(&fake.font,"LABEL",50,6,{255,255,255,255},true,100,false,false,true);
 assert(drawn.back().y==0);assert(drawn.back().h==12);
 for(const auto& align:{"left","center","right"}){drawn.clear();fake.drawWrappedText(&fake.font,"LABEL",{0,0,100,30},"top",align,{255,255,255,255});assert(!drawn.empty());assert(drawn.back().y>=0);assert(drawn.back().y+drawn.back().h<=30);}
 // Layout holds the state reference but does not access it during these geometry checks.
 const int mockState=0;
 DeckButton s;s.widgetType="Slider";s.sliderShowValue=true;s.sliderMin=0;s.sliderMax=100;s.sliderLevel=1;
 Layout sl{fake,s,mockState,cfg,100,1.0,1000};auto sb=sl.layout(slider,{0,0,100,300});
 auto* track=findBox(sb,"slider.track");auto* percent=findBox(sb,"slider.value");assert(track&&percent);assert(percent->area.y>=track->area.y+track->area.h);assert(percent->area.y+percent->area.h<=300);
 s.sliderShowValue=false;s.sliderValueText="100 %";
 assert(sl.label(*value,(*value)["properties"],"ui.text").empty());
 auto gauges=read("tests/fixtures/Gauges-tree.json")["tree"]["root"];DeckButton g;g.widgetType="Gauges";g.width=2;g.height=1;
 Layout gl{fake,g,mockState,cfg,100,1.0,1000};auto gb=gl.layout(gauges,{0,0,220,100});
 auto* ring=findBox(gb,"gauges.layout.c3-grid.g0.dial.ring");assert(ring);assert(ring->area.w>50);assert(ring->area.h>30);
 auto* last=findBox(gb,"gauges.layout.c3-grid.g2.dial.ring");assert(last);assert(last->area.y==ring->area.y);assert(last->area.w==ring->area.w||std::abs(last->area.w-ring->area.w)<=1);
 g.width=1;g.height=2;
 Layout tall{fake,g,mockState,cfg,100,100.0/120,1000};auto tallBox=tall.layout(gauges,{0,0,100,213});
 const auto* topGauge=findBox(tallBox,"gauges.layout.c1-grid.g0.dial.ring");
 const auto* bottomGauge=findBox(tallBox,"gauges.layout.c1-grid.g2.dial.ring");
 assert(topGauge && bottomGauge && topGauge->area.x==bottomGauge->area.x && topGauge->area.y<bottomGauge->area.y);
 const Json fontProps={{"size",{{"basis",0.28},{"maxOfCross",0.50}}},{"fontFace","arial-400-5-upright"},{"text","Test"}};
 assert(tall.fontSize(fontProps,{0,0,100,80})==28);
 assert(tall.fontSize(fontProps,{0,0,100,12},40)==20);
 tall.measure({{"type","ui.text"},{"properties",fontProps}},{0,0,100,80});
 assert(fake.fontFace=="arial-400-5-upright");
 auto battery=read("tests/fixtures/Battery-tree.json")["tree"]["root"];auto* glyph=find(battery,"battery.single.square.dial.centre.glyph");assert(glyph);assert(widgetIconName((*glyph)["properties"])=="device-laptop");
 std::cout<<"PASS: widget recovery and command feedback, artwork, config recovery, ping, widget timing and layout\n";
}
