#include "ui_renderer.hpp"
#include "widget_localization.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <SDL2/SDL2_gfxPrimitives.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {
using Json = nlohmann::json;
const Json emptyObject = Json::object();
const Json emptyArray = Json::array();

const Json& object(const Json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_object() ? *it : emptyObject;
}

const Json& children(const Json& value) {
    const auto it = value.find("children");
    return it != value.end() && it->is_array() ? *it : emptyArray;
}

SDL_Color colorOf(const std::string& value, SDL_Color fallback) {
    if (value == "transparent" || value == "none") return {0, 0, 0, 0};
    if ((value.size() != 7 && value.size() != 9) || value.front() != '#') return fallback;
    if (value.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) return fallback;
    const auto bits = std::strtoul(value.c_str() + 1, nullptr, 16);
    const auto rgba = value.size() == 7 ? (bits << 8) | 255 : bits;
    return {static_cast<Uint8>(rgba >> 24), static_cast<Uint8>(rgba >> 16),
            static_cast<Uint8>(rgba >> 8), static_cast<Uint8>(rgba)};
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

bool declaresEvent(const Json& node, const std::string& event) {
    for (const auto* owner : {&node, &object(node, "properties")}) {
        const auto events = owner->find("events");
        if (events != owner->end() && events->is_array() &&
            std::find(events->begin(), events->end(), event) != events->end()) return true;
    }
    return false;
}
constexpr double radians = 3.14159265358979323846 / 180;

SDL_Point onArc(int x, int y, int radius, double angle) {
    return {x + static_cast<int>(std::lround(radius * std::sin(angle * radians))),
            y - static_cast<int>(std::lround(radius * std::cos(angle * radians)))};
}

bool textComponent(const std::string& type) {
    return type == "ui.text" || type == "macrodeck.progress-text" || type == "macrodeck.dynamic-text";
}

bool levelComponent(const std::string& type) {
    return type == "ui.slider" || type == "macrodeck.slider" || type == "ui.dial";
}

struct Arc { double start; double sweep; };
Arc arcOf(const Json& props) {
    const double start = std::remainder(jsonDouble(props, "startAngle", -135), 360.0);
    const double sweep = jsonDouble(props, "endAngle", 135) - jsonDouble(props, "startAngle", -135);
    return {start, std::clamp(sweep, -360.0, 360.0)};
}

struct WallTime {
    std::tm calendar{};
    int offset = 0;
    bool named = false;
};

WallTime readWallTime(const std::string& zone) {
    const auto epoch = std::time(nullptr);
    WallTime value;
#ifdef __SWITCH__
    struct Entry {
        std::time_t last = -1;
        WallTime time;
        TimeZoneRule rule{};
        bool hasRule = false;
    };
    static std::map<std::string, Entry> clocks;
    auto found = clocks.find(zone);
    if (found == clocks.end()) {
        if (clocks.size() >= 12) clocks.erase(clocks.begin());
        found = clocks.try_emplace(zone).first;
        TimeLocationName location{};
        if (!zone.empty() && zone.size() < sizeof(location.name) && R_SUCCEEDED(timeInitialize())) {
            std::snprintf(location.name, sizeof(location.name), "%s", zone.c_str());
            found->second.hasRule = R_SUCCEEDED(timeLoadTimeZoneRule(&location, &found->second.rule));
            timeExit();
        }
    }
    auto& cached = found->second;
    if (cached.last == epoch && epoch != -1) return cached.time;
    if (epoch >= 0 && R_SUCCEEDED(timeInitialize())) {
        TimeCalendarTime local{};
        TimeCalendarAdditionalInfo info{};
        const auto result = cached.hasRule ? timeToCalendarTime(&cached.rule, epoch, &local, &info)
                                            : timeToCalendarTimeWithMyRule(epoch, &local, &info);
        timeExit();
        if (R_SUCCEEDED(result)) {
            value.calendar.tm_year = local.year - 1900;
            value.calendar.tm_mon = local.month - 1;
            value.calendar.tm_mday = local.day;
            value.calendar.tm_hour = local.hour;
            value.calendar.tm_min = local.minute;
            value.calendar.tm_sec = local.second;
            value.calendar.tm_wday = info.wday;
            value.calendar.tm_yday = info.yday;
            value.offset = info.offset;
            value.named = cached.hasRule;
            cached.last = epoch;
            cached.time = value;
            return value;
        }
    }
#else
    (void)zone;
#endif
    if (const auto* local = std::localtime(&epoch)) value.calendar = *local;
    return value;
}

std::string clockZone(const Json& props) {
    return jsonStr(object(object(props, "value"), "$time"), "zone");
}

std::string dynamicLabel(const Json& props, const DeckButton& button, int language) {
    const auto zone = clockZone(props);
    const auto current = readWallTime(zone);
    const auto format = jsonStr(props, "format", "time");
    if (format == "zone-name") {
        if (zone.empty()) return {};
        auto name = zone.substr(zone.find_last_of('/') + 1);
        std::replace(name.begin(), name.end(), '_', ' ');
        return name;
    }
    char output[96]{};
    if (format == "zone-offset") {
        if (!current.named) return {};
        const auto minutes = std::abs(current.offset / 60);
        std::snprintf(output, sizeof(output), "UTC%c%02d:%02d", current.offset < 0 ? '-' : '+',
                      minutes / 60, minutes % 60);
        return output;
    }
    const auto& calendar = current.calendar;
    if (format.rfind("date", 0) == 0) return formatClockDate(calendar, format, language);
    if (format != "time" && format != "time-12h" && format != "time-12h-padded" &&
        format != "time-24h" && format != "time-24h-unpadded") return {};
    const bool twelve = format == "time-12h" || format == "time-12h-padded";
    const int hour = twelve ? (calendar.tm_hour + 11) % 12 + 1 : calendar.tm_hour;
    const bool padded = format != "time-12h" && format != "time-24h-unpadded";
    const bool seconds = jsonBool(props, "seconds", button.clockShowSeconds);
    const int pos = std::snprintf(output, sizeof(output), padded ? "%02d:%02d" : "%d:%02d", hour, calendar.tm_min);
    if (pos < 0 || pos >= static_cast<int>(sizeof(output))) return {};
    std::string text(output);
    if (seconds) {
        std::snprintf(output, sizeof(output), ":%02d", calendar.tm_sec);
        text += output;
    }
    if (twelve) text += calendar.tm_hour < 12 ? " AM" : " PM";
    return text;
}
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

bool UIRenderer::hasWidgetFont(const std::string& face) const {
    return widgetFontBytes_.count(face) != 0;
}

void UIRenderer::setWidgetFontBytes(const std::string& face, const std::string& bytes) {
    if (hasWidgetFont(face)) return;
    auto& stored = widgetFontBytes_[face];
    if (bytes.empty() || bytes.size() > 12 * 1024 * 1024 - widgetFontBytesTotal_) return;
    auto* stream = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()));
    if (!stream) return;
    auto* font = TTF_OpenFontRW(stream, 1, 16);
    if (!font) return;
    TTF_CloseFont(font);
    stored = bytes;
    widgetFontBytesTotal_ += stored.size();
}

void UIRenderer::clearWidgetFontBytes() {
    clearRenderCaches();
    for (auto& entry : widgetFonts_) TTF_CloseFont(entry.second);
    widgetFonts_.clear();
    widgetFontBytes_.clear();
    widgetFontBytesTotal_ = 0;
    setDefaultWidgetFonts({}, {});
}

void UIRenderer::setDefaultWidgetFonts(const std::string& regular, const std::string& bold) {
    defaultWidgetFont_ = regular;
    defaultBoldWidgetFont_ = bold;
}

TTF_Font* UIRenderer::widgetFont(int size, bool bold, const std::string& face) {
    const int pixels = std::clamp(size, 4, 160);
    const auto& preferred = !face.empty() ? face :
        (bold && !defaultBoldWidgetFont_.empty() ? defaultBoldWidgetFont_ : defaultWidgetFont_);
    const auto remote = widgetFontBytes_.find(preferred);
    const bool custom = remote != widgetFontBytes_.end() && !remote->second.empty();
    const bool syntheticBold = bold && (!custom || !face.empty() || preferred != defaultBoldWidgetFont_);
    const auto key = std::make_tuple(pixels, syntheticBold, custom ? preferred : std::string{});
    const auto cached = widgetFonts_.find(key);
    if (cached != widgetFonts_.end()) return cached->second;
    // Pixel sizes vary with the panel grid. Keep the number of open faces bounded.
    if (widgetFonts_.size() >= 48) {
        clearRenderCaches();
        for (auto& entry : widgetFonts_) TTF_CloseFont(entry.second);
        widgetFonts_.clear();
    }
    TTF_Font* font = nullptr;
    if (custom) {
        const auto& bytes = remote->second;
        if (auto* stream = SDL_RWFromConstMem(bytes.data(), static_cast<int>(bytes.size()))) {
            font = TTF_OpenFontRW(stream, 1, pixels);
        }
    }
    if (!font && !widgetFontPath_.empty()) font = TTF_OpenFont(widgetFontPath_.c_str(), pixels);
    if (!font) return fontNormal_;
    if (syntheticBold) TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    widgetFonts_.emplace(key, font);
    return font;
}

struct UIRenderer::WidgetTree {
    struct Box {
        const Json* node = nullptr;
        SDL_Rect area{};
        std::vector<Box> child;
        bool supported = true;
        bool textFits = true;
        int parentCross = 0;
    };

    UIRenderer& ui;
    const DeckButton& button;
    const DeckState& state;
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

    SDL_Color accent() const {
        return button.hasAccentColor ? SDL_Color{button.accentR, button.accentG, button.accentB, 255}
                                    : SDL_Color{33, 150, 243, 255};
    }

    void image(const std::string& id, const Json& props, SDL_Rect area, double opacity, double scale = 1) {
        int w = 0, h = 0;
        auto* texture = state.getResourceTexture(id, &w, &h, now);
        if (id.empty() && button.widgetType == "ActionButton" && !button.iconBase64.empty()) {
            texture = button.iconTexture;
            w = button.iconWidth;
            h = button.iconHeight;
        }
        if (!texture || w <= 0 || h <= 0 || area.w <= 0 || area.h <= 0) return;
        const bool cover = jsonStr(props, "fit") == "cover";
        const double xScale = static_cast<double>(area.w) / w;
        const double yScale = static_cast<double>(area.h) / h;
        const double fit = (cover ? std::max(xScale, yScale) : std::min(xScale, yScale)) *
                           std::clamp(jsonDouble(props, "zoom", 1), 0.1, 4.0) * scale;
        const int width = std::max(1, static_cast<int>(std::round(w * fit)));
        const int height = std::max(1, static_cast<int>(std::round(h * fit)));
        SDL_Rect dest{area.x + (area.w - width) / 2, area.y + (area.h - height) / 2, width, height};
        dest.x += static_cast<int>(area.w * std::clamp(jsonDouble(props, "offsetX"), -1.0, 1.0));
        dest.y += static_cast<int>(area.h * std::clamp(jsonDouble(props, "offsetY"), -1.0, 1.0));
        SDL_Rect previous{};
        SDL_RenderGetClipRect(ui.renderer_, &previous);
        SDL_Rect clipped{};
        if (!SDL_IntersectRect(&area, &previous, &clipped)) return;
        SDL_RenderSetClipRect(ui.renderer_, &clipped);
        auto tint = colorOf(jsonStr(props, "tint"), {255, 255, 255, 255});
        const double brightness = std::clamp(jsonDouble(props, "brightness", 1), 0.0, 2.0);
        auto channel = [&](Uint8 value) { return static_cast<Uint8>(std::min(255.0, value * brightness)); };
        SDL_SetTextureColorMod(texture, channel(tint.r), channel(tint.g), channel(tint.b));
        SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(std::clamp(opacity, 0.0, 1.0) * tint.a));
        const bool spinningDisc = id == "app.macro-deck.music-player.disc-spinning";
        if (spinningDisc) {
            SDL_RenderCopyEx(ui.renderer_, texture, nullptr, &dest, (now % 2400) * 360.0 / 2400.0,
                             nullptr, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopy(ui.renderer_, texture, nullptr, &dest);
        }
        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, 255);
        SDL_RenderSetClipRect(ui.renderer_, &previous);
    }

    void artwork(const Json& node, const Json& props, SDL_Rect area, double inheritedOpacity) {
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

    float nodeLevel(const Json& node, const Json& props) const {
        const auto id = jsonStr(node, "id");
        if ((!id.empty() && id == button.sliderNodeId && button.widgetType == "Slider") ||
            (!id.empty() && id == button.activeLevelNode &&
             (button.isPressed || now - button.lastLocalSlideTick < 350))) {
            return std::clamp(button.visualSliderLevel < 0 ? button.sliderLevel : button.visualSliderLevel, 0.0f, 1.0f);
        }
        return static_cast<float>(std::clamp(jsonDouble(props, "level", jsonDouble(props, "value")), 0.0, 1.0));
    }

    int trackThickness(const Json& props, SDL_Rect area, bool vertical) const {
        const int cross = std::max(1, vertical ? area.w : area.h);
        return std::clamp(length(props, "thickness", area, std::max(2, basis / 22), cross), 1, cross);
    }

    void paintTrack(const Json& node, const Json& props, SDL_Rect area, double opacity, const std::string& type) {
        const bool slider = levelComponent(type);
        const bool vertical = jsonStr(props, "direction") == "vertical";
        const int thickness = trackThickness(props, area, vertical);
        const auto shade = [opacity](SDL_Color color) {
            color.a = static_cast<Uint8>(color.a * opacity);
            return color;
        };
        const SDL_Rect track = vertical ? SDL_Rect{area.x + (area.w - thickness) / 2, area.y, thickness, area.h}
                                        : SDL_Rect{area.x, area.y + (area.h - thickness) / 2, area.w, thickness};
        const auto trackColor = shade(colorOf(jsonStr(props, "trackColor"), {107, 107, 112, 130}));
        UIRenderer::drawRoundedBox(ui.renderer_, track, thickness / 2, trackColor);
        double start = type == "ui.range-bar" ? std::clamp(jsonDouble(props, "start"), 0.0, 1.0) : 0.0;
        double finish = type == "ui.range-bar" ? std::clamp(jsonDouble(props, "end", start), 0.0, 1.0) :
                        slider ? nodeLevel(node, props) : button.getCurrentProgressFraction(now);
        if (finish < start) std::swap(start, finish);
        const int span = vertical ? track.h : track.w;
        const int from = static_cast<int>(std::lround(start * span));
        const int to = static_cast<int>(std::lround(finish * span));
        const SDL_Rect filled = vertical ? SDL_Rect{track.x, track.y + span - to, track.w, to - from}
                                         : SDL_Rect{track.x + from, track.y, to - from, track.h};
        const auto color = shade(colorOf(jsonStr(props, "levelColor",
            jsonStr(props, "endColor", jsonStr(props, "startColor", jsonStr(props, "color")))), accent()));
        UIRenderer::drawRoundedBox(ui.renderer_, filled, thickness / 2, color);
        const auto marker = props.find("marker");
        if (slider || (marker != props.end() && marker->is_number())) {
            const double fraction = slider ? finish : std::clamp(marker->get<double>(), 0.0, 1.0);
            const int radius = std::max(2, static_cast<int>(std::lround(thickness * 1.15)));
            const int extent = std::max(0, span - 2 * radius);
            const int pos = std::min(radius, span / 2) + static_cast<int>(std::lround(fraction * extent));
            const int x = vertical ? track.x + track.w / 2 : track.x + pos;
            const int y = vertical ? track.y + span - pos : track.y + track.h / 2;
            UIRenderer::drawFilledCircleAA(ui.renderer_, x, y, radius,
                shade(colorOf(jsonStr(props, "markerColor"), {246, 246, 248, 255})));
        }
    }

    void paintGauge(const Json& node, const Json& props, SDL_Rect area, double opacity, bool dial) {
        const int diameter = std::min(area.w, area.h);
        if (diameter < 5) return;
        const int thickness = std::clamp(length(props, "thickness", area, std::max(2, diameter / 14)), 1, diameter / 2);
        const int radius = (diameter - thickness) / 2;
        const int x = area.x + area.w / 2;
        const int y = area.y + area.h / 2;
        const auto shade = [opacity](SDL_Color color) {
            color.a = static_cast<Uint8>(color.a * opacity);
            return color;
        };
        const auto angles = arcOf(props);
        const double start = angles.start;
        const double sweep = angles.sweep;
        const auto arc = [&](double end, SDL_Color color) {
            UIRenderer::drawSmoothArcAA(ui.renderer_, x, y, radius, thickness,
                static_cast<float>(std::min(start, end) - 90), static_cast<float>(std::max(start, end) - 90), shade(color));
        };
        arc(start + sweep, colorOf(jsonStr(props, "trackColor"), {107, 107, 112, 140}));
        float fraction = nodeLevel(node, props);
        const auto id = jsonStr(node, "id");
        const auto endPart = id.find_last_of("./:");
        const auto leaf = endPart == std::string::npos ? id : id.substr(endPart + 1);
        if (leaf == "ring" && button.progressDurationMs > 0) {
            if (button.widgetType == "Countdown") fraction = 1.0f - button.getCurrentProgressFraction(now);
            else if (button.widgetType == "Stopwatch") fraction = button.getCurrentProgressFraction(now);
        }
        const double end = start + sweep * fraction;
        if (fraction > 0) arc(end, colorOf(jsonStr(props, "levelColor", jsonStr(props, "color")), accent()));
        if (dial) {
            const auto tip = onArc(x, y, radius, end);
            UIRenderer::drawFilledCircleAA(ui.renderer_, tip.x, tip.y, std::max(2, thickness * 5 / 4),
                shade({246, 246, 248, 255}), shade({31, 31, 34, 255}), std::max(1, thickness / 4));
        }
    }

    void paintChart(const Json& props, SDL_Rect area, double opacity) {
        const auto series = props.find("points");
        if (series == props.end() || !series->is_array() || series->empty()) return;
        std::vector<double> values;
        const size_t count = std::min<size_t>(series->size(), 4096);
        for (auto it = series->end() - count; it != series->end(); ++it) {
            if (it->is_number()) values.push_back(std::clamp(it->get<double>(), 0.0, 1.0));
        }
        if (values.empty()) return;
        const int top = area.y + static_cast<int>(std::clamp(jsonDouble(props, "plotTop"), 0.0, 1.0) * area.h);
        const int bottom = area.y + area.h - 1;
        const int thickness = std::clamp(length(props, "thickness", area, 2), 1, 24);
        auto color = colorOf(jsonStr(props, "color"), accent());
        SDL_Color fill = color;
        fill.a = static_cast<Uint8>(std::lround(fill.a * opacity * 0.16));
        color.a = static_cast<Uint8>(std::lround(color.a * opacity * 0.9));
        const auto yAt = [&](double value) {
            return bottom - static_cast<int>(std::lround(value * std::max(0, bottom - top)));
        };
        SDL_Point previous{area.x, yAt(values.front())};
        if (values.size() == 1) {
            UIRenderer::drawFilledCircleAA(ui.renderer_, area.x + area.w / 2, previous.y, thickness, color);
            return;
        }
        std::vector<SDL_Vertex> vertices;
        const int samples = std::min<int>(area.w, values.size());
        if (samples < 2) return;
        vertices.reserve((samples - 1) * 6);
        std::vector<SDL_Point> points;
        points.reserve(samples);
        points.push_back(previous);
        for (int i = 1; i < samples; ++i) {
            const auto pick = static_cast<size_t>(i) * (values.size() - 1) / (samples - 1);
            SDL_Point next{area.x + i * (area.w - 1) / (samples - 1), yAt(values[pick])};
            const SDL_Vertex a{{static_cast<float>(previous.x), static_cast<float>(previous.y)}, fill, {}};
            const SDL_Vertex b{{static_cast<float>(next.x), static_cast<float>(next.y)}, fill, {}};
            const SDL_Vertex c{{static_cast<float>(previous.x), static_cast<float>(bottom)}, fill, {}};
            const SDL_Vertex d{{static_cast<float>(next.x), static_cast<float>(bottom)}, fill, {}};
            vertices.insert(vertices.end(), {a, b, c, c, b, d});
            points.push_back(next);
            previous = next;
        }
        SDL_SetRenderDrawBlendMode(ui.renderer_, SDL_BLENDMODE_BLEND);
        SDL_RenderGeometry(ui.renderer_, nullptr, vertices.data(), static_cast<int>(vertices.size()), nullptr, 0);
        for (size_t i = 1; i < points.size(); ++i) {
            thickLineRGBA(ui.renderer_, points[i - 1].x, points[i - 1].y, points[i].x, points[i].y,
                          thickness, color.r, color.g, color.b, color.a);
        }
        SDL_SetRenderDrawBlendMode(ui.renderer_, SDL_BLENDMODE_BLEND);
    }

    void paintClock(const Json& props, SDL_Rect area, double opacity) {
        const int radius = std::min(area.w, area.h) / 2;
        if (radius < 5) return;
        const int x = area.x + area.w / 2;
        const int y = area.y + area.h / 2;
        const auto calendar = readWallTime(clockZone(props)).calendar;
        auto marks = colorOf(jsonStr(props, "color"), config.theme == 1 ? SDL_Color{33, 33, 35, 255}
                                                                       : SDL_Color{243, 243, 245, 255});
        auto secondColor = accent();
        marks.a = static_cast<Uint8>(marks.a * opacity);
        secondColor.a = static_cast<Uint8>(secondColor.a * opacity);
        const auto line = [&](SDL_Point a, SDL_Point b, int width, SDL_Color color) {
            thickLineRGBA(ui.renderer_, a.x, a.y, b.x, b.y, width, color.r, color.g, color.b, color.a);
        };
        for (int tick = 0; tick < 12; ++tick) {
            const float angle = tick * 30.0f;
            const bool cardinal = tick % 3 == 0;
            line(onArc(x, y, radius * (cardinal ? 75 : 81) / 100, angle), onArc(x, y, radius * 91 / 100, angle),
                 std::max(1, radius / (cardinal ? 20 : 32)), marks);
        }
        line({x, y}, onArc(x, y, radius * 48 / 100, calendar.tm_hour * 30 + calendar.tm_min * 0.5),
             std::max(2, radius / 11), marks);
        line({x, y}, onArc(x, y, radius * 71 / 100, calendar.tm_min * 6 + calendar.tm_sec * 0.1),
             std::max(1, radius / 16), marks);
        if (jsonBool(props, "seconds", button.clockShowSeconds)) {
            line({x, y}, onArc(x, y, radius * 82 / 100, calendar.tm_sec * 6),
                std::max(1, radius / 30), secondColor);
        }
        UIRenderer::drawFilledCircleAA(ui.renderer_, x, y, std::max(2, radius / 12), marks);
        SDL_SetRenderDrawBlendMode(ui.renderer_, SDL_BLENDMODE_BLEND);
    }

    SDL_Color hue(double phase, Uint8 alpha) const {
        double h = (phase - std::floor(phase)) * 6;
        const auto value = [&](int offset) {
            const double x = std::fmod(h + offset, 6.0);
            return static_cast<Uint8>(255 * std::clamp(std::abs(x - 3.0) - 1.0, 0.0, 1.0));
        };
        return {value(0), value(4), value(2), alpha};
    }

    void buttonRing(const Json& props, SDL_Rect area, int radius, double opacity) {
        const auto style = jsonStr(props, "borderStyle", button.borderStyle);

        if (style.empty() || style == "none" || style == "off") return;
        const int thickness = std::max(2, static_cast<int>(std::lround(2 * cellScale)));
        const SDL_Color configured{button.borderR, button.borderG, button.borderB, 255};
        auto color = colorOf(jsonStr(props, "borderColor"), button.hasBorder ? configured : accent());
        double alpha = opacity;
        Uint64 period = 4800;
        if (style == "heartbeat") period = 2600;
        else if (style == "blink") period = 2200;
        else if (style == "breathing" || style == "comet") period = 3600;
        else if (style == "ants") period = 12000;
        else if (style == "hue-shift") period = 8000;
        const auto phase = (now % period) / static_cast<double>(period);
        if (style == "heartbeat") {
            const auto pulse = [&](double at) { return std::max(0.0, 1 - std::abs(phase - at) * 15); };
            alpha *= 0.25 + 0.75 * std::max(pulse(0.11), pulse(0.25) * 0.8);
        } else if (style == "breathing") {
            alpha *= 0.28 + 0.72 * (0.5 - 0.5 * std::cos(phase * 360 * radians));
        } else if (style == "blink") {
            alpha *= phase < 0.5 ? 1 : 0.16;
        } else if (style == "hue-shift") {
            color = hue(phase, color.a);
        }
        color.a = static_cast<Uint8>(std::clamp(alpha, 0.0, 1.0) * color.a);
        if (style != "comet" && style != "ants" && style != "rgb") {
            UIRenderer::drawRoundedBorder(ui.renderer_, area, radius, thickness, color);
            return;
        }
        const double padding = thickness * 0.5 + 0.5;
        const double width = std::max(1.0, area.w - padding * 2);
        const double height = std::max(1.0, area.h - padding * 2);
        const double r = std::clamp(radius - padding, 0.0, std::min(width, height) * 0.5);
        const double sx = width - 2 * r, sy = height - 2 * r, corner = 90 * radians * r;
        const double perimeter = 2 * (sx + sy) + 4 * corner;
        if (perimeter <= 0) return;
        const auto pointAt = [&](double distance) {
            double t = distance;
            const double x = area.x + padding, y = area.y + padding;
            const auto point = [](double px, double py) {
                return SDL_Point{static_cast<int>(std::lround(px)), static_cast<int>(std::lround(py))};
            };
            const auto turn = [&](double cx, double cy, double angle) {
                return point(cx + r * std::cos(angle * radians), cy + r * std::sin(angle * radians));
            };
            if (t < sx) return point(x + r + t, y);
            t -= sx;
            if (t < corner) return turn(x + width - r, y + r, -90 + t / corner * 90);
            t -= corner;
            if (t < sy) return point(x + width, y + r + t);
            t -= sy;
            if (t < corner) return turn(x + width - r, y + height - r, t / corner * 90);
            t -= corner;
            if (t < sx) return point(x + width - r - t, y + height);
            t -= sx;
            if (t < corner) return turn(x + r, y + height - r, 90 + t / corner * 90);
            t -= corner;
            if (t < sy) return point(x, y + height - r - t);
            t -= sy;
            return corner > 0 ? turn(x + r, y + r, 180 + t / corner * 90) : point(x, y);
        };
        const int segments = std::clamp(static_cast<int>(perimeter / 3), 24, 240);
        auto last = pointAt(0);
        for (int i = 1; i <= segments; ++i) {
            const double u = static_cast<double>(i) / segments;
            const auto p = pointAt(i == segments ? 0 : u * perimeter);
            const double local = u - phase + 1;
            auto tint = color;
            if (style == "rgb") tint = hue(local, color.a);
            else if (style == "ants" && std::fmod(local * 16, 1) > 0.55) tint.a = 0;
            else if (style == "comet") {
                const double distance = std::fmod(local, 1);
                tint.a = static_cast<Uint8>(tint.a * (0.10 + 0.90 * std::max(0.0, 1 - distance * 6)));
            }
            if (tint.a) thickLineRGBA(ui.renderer_, last.x, last.y, p.x, p.y, thickness, tint.r, tint.g, tint.b, tint.a);
            last = p;
        }
        SDL_SetRenderDrawBlendMode(ui.renderer_, SDL_BLENDMODE_BLEND);
    }

    void paint(const Box& box, double inheritedOpacity = 1, bool root = false) {
        if (!box.node || box.area.w <= 0 || box.area.h <= 0) return;
        const auto& node = *box.node;
        const auto& props = object(node, "properties");
        const auto& mods = object(props, "modifiers");
        const auto type = jsonStr(node, "type");
        const SDL_Rect area = box.area;
        SDL_Rect outerClip{}, visible{};
        SDL_RenderGetClipRect(ui.renderer_, &outerClip);
        if (!SDL_IntersectRect(&area, &outerClip, &visible)) return;
        SDL_RenderSetClipRect(ui.renderer_, &visible);
        const double opacity = inheritedOpacity * std::clamp(jsonDouble(props, "opacity", 1), 0.0, 1.0);
        const auto withOpacity = [&](SDL_Color c) {
            c.a = static_cast<Uint8>(std::round(c.a * opacity));
            return c;
        };
        const auto bg = jsonStr(mods, "background", jsonStr(props, "background", jsonStr(props, "backgroundColor")));
        const int corner = type != "ui.button" ? 0 :
            (root ? tileRadius : static_cast<int>(std::lround(area.h * std::clamp(jsonDouble(props, "corner", 0.12), 0.0, 0.5))));
        const int radius = length(mods, "radius", area, corner);
        if (!bg.empty() || type == "ui.button") {
            const auto background = colorOf(bg, type == "ui.button" ? accent() : SDL_Color{0, 0, 0, 0});
            UIRenderer::drawRoundedBox(ui.renderer_, area, radius, withOpacity(background));
        }
        if (type == "ui.image" || type == "ui.button") {
            SDL_Rect target = area;
            if (type == "ui.image" && props.contains("size")) {
                const int size = std::min({length(props, "size", area), area.w, area.h});
                target = {area.x + (area.w - size) / 2, area.y + (area.h - size) / 2, size, size};
            }
            artwork(node, props, target, inheritedOpacity);
        }
        if (textComponent(type)) {
            const auto text = label(node, props, type);
            const auto face = jsonStr(props, "fontFace");
            int size = std::min(fontSize(props, area, box.parentCross), std::max(4, area.h));
            auto* font = ui.widgetFont(size, bold(props), face);
            while (font && size > 4 && TTF_FontHeight(font) > area.h) {
                font = ui.widgetFont(--size, bold(props), face);
            }
            const int minSize = std::clamp(length(props, "minSize", area, size, box.parentCross), 4, size);
            while (size > minSize && !jsonBool(props, "wrap") && ui.getTextWidth(font, text) > area.w) {
                font = ui.widgetFont(--size, bold(props), face);
            }
            const bool light = config.theme == 1;
            const auto role = jsonStr(props, "role");
            SDL_Color foreground = light ? SDL_Color{26, 30, 36, 255} : SDL_Color{245, 245, 245, 255};
            if (role == "secondary" || role == "muted") {
                foreground = light ? SDL_Color{88, 94, 105, 255} : SDL_Color{172, 172, 179, 255};
            }
            foreground = withOpacity(colorOf(jsonStr(props, "color"), foreground));
            const auto align = jsonStr(props, "align");
            const bool center = align == "center";
            const bool end = align == "end" || align == "right";
            if (jsonBool(props, "wrap")) {
                const int line = font ? std::max(1, TTF_FontLineSkip(font)) : size;
                const int lines = std::clamp(jsonInt(props, "maxLines", area.h / line), 1, 32);
                SDL_Rect textArea = area;
                textArea.h = std::min(area.h, lines * line);
                textArea.y += (area.h - textArea.h) / 2;
                ui.drawWrappedText(font, text, textArea, "center", center ? "center" : (end ? "right" : "left"), foreground);
            } else {
                ui.drawText(font, text, area.x + (center ? area.w / 2 : (end ? area.w : 0)), area.y + area.h / 2,
                            foreground, center, area.w, end, false, true);
            }
        } else if (type == "ui.shape") {
            const auto shape = jsonStr(props, "shape");
            const auto fill = withOpacity(colorOf(jsonStr(props, "color", jsonStr(props, "fill")), accent()));
            if (shape == "circle") {
                UIRenderer::drawFilledCircleAA(ui.renderer_, area.x + area.w / 2, area.y + area.h / 2,
                    std::min(area.w, area.h) / 2, fill);
            } else {
                UIRenderer::drawRoundedBox(ui.renderer_, area, length(props, "radius", area), fill);
            }
        } else if (type == "ui.icon") {
            const int size = std::min({area.w, area.h, length(props, "size", area, basis / 4)});
            const auto icon = widgetIconName(props);
            auto foreground = config.theme == 1 ? SDL_Color{33, 38, 45, 255} : SDL_Color{255, 255, 255, 255};
            ui.drawVectorIcon(icon, area.x + area.w / 2, area.y + area.h / 2, size,
                              withOpacity(colorOf(jsonStr(props, "color"), foreground)));
        } else if (type == "ui.gauge" || type == "ui.dial") {
            paintGauge(node, props, area, opacity, type == "ui.dial");
        } else if (type == "ui.chart") {
            paintChart(props, area, opacity);
        } else if (type == "macrodeck.clock-dial") {
            paintClock(props, area, opacity);
        } else if (type == "macrodeck.progress-bar" || type == "ui.range-bar" || levelComponent(type)) {
            paintTrack(node, props, area, opacity, type);
        }
        SDL_Rect oldClip{};
        SDL_RenderGetClipRect(ui.renderer_, &oldClip);
        SDL_Rect childClip{};
        if (SDL_IntersectRect(&area, &oldClip, &childClip)) {
            SDL_RenderSetClipRect(ui.renderer_, &childClip);
            for (const auto& child : box.child) {
                paint(child, type == "ui.modifier" ? opacity : inheritedOpacity);
            }
            SDL_RenderSetClipRect(ui.renderer_, &oldClip);
        }
        const int borderWidth = length(mods, "borderWidth", area);
        if (borderWidth > 0) {
            UIRenderer::drawRoundedBorder(ui.renderer_, area, radius, borderWidth,
                withOpacity(colorOf(jsonStr(mods, "borderColor"), {255, 255, 255, 255})));
        }
        const auto id = jsonStr(node, "id");
        const bool tileButton = root && (button.widgetType == "ActionButton" || button.widgetType == "Countdown" ||
                                         button.widgetType == "Stopwatch");
        if (type == "ui.button" && !id.empty() && (button.pressedUiNode == id || tileButton) &&
            button.visualPressFraction > 0) {
            UIRenderer::drawRoundedBox(ui.renderer_, area, radius,
                {255, 255, 255, static_cast<Uint8>(std::lround(51 * button.visualPressFraction))});
        }
        if (type == "ui.button" && (root || props.contains("borderStyle"))) buttonRing(props, area, radius, opacity);
        SDL_RenderSetClipRect(ui.renderer_, &outerClip);
    }

    std::optional<LevelControl> levelAt(const Box& box, SDL_Point pointer) const {
        if (!box.node || !SDL_PointInRect(&pointer, &box.area)) return std::nullopt;
        for (auto it = box.child.rbegin(); it != box.child.rend(); ++it) {
            if (auto hit = levelAt(*it, pointer)) return hit;
        }
        const auto type = jsonStr(*box.node, "type");
        const auto id = jsonStr(*box.node, "id");
        const auto& props = object(*box.node, "properties");
        if (!levelComponent(type) || id.empty() || jsonBool(props, "disabled")) return std::nullopt;
        const bool adjust = declaresEvent(*box.node, "adjust");
        const bool change = declaresEvent(*box.node, "change");
        const bool doublePress = declaresEvent(*box.node, "double-press");
        const bool stockSlider = button.widgetType == "Slider" && id == button.sliderNodeId;
        if (!adjust && !change && !doublePress && !stockSlider) return std::nullopt;
        const auto [start, sweep] = arcOf(props);
        return LevelControl{id, box.area, jsonStr(props, "direction") == "vertical",
                            jsonStr(props, "interaction") == "relative", type == "ui.dial",
                            adjust || (stockSlider && !change), change || (stockSlider && !adjust), doublePress,
                            nodeLevel(*box.node, props), static_cast<float>(std::clamp(jsonDouble(props, "step"), 0.0, 1.0)),
                            start, sweep};
    }

    bool hasVisible(const Box& box, const std::function<bool(const Json&, SDL_Rect)>& matches) const {
        if (!box.node || box.area.w <= 0 || box.area.h <= 0) return false;
        if (matches(*box.node, box.area)) return true;
        return std::any_of(box.child.begin(), box.child.end(), [&](const Box& child) { return hasVisible(child, matches); });
    }

    std::string buttonAt(const Box& box, SDL_Point point) const {
        if (!box.node || !SDL_PointInRect(&point, &box.area)) return {};
        for (auto it = box.child.rbegin(); it != box.child.rend(); ++it) {
            auto id = buttonAt(*it, point);
            if (!id.empty()) return id;
        }
        if (jsonStr(*box.node, "type") == "ui.button" && declaresEvent(*box.node, "press") &&
            !jsonBool(object(*box.node, "properties"), "disabled")) return jsonStr(*box.node, "id");
        return {};
    }
};

bool UIRenderer::renderWidgetTree(const DeckButton& btn, const SDL_Rect& rect,
                                  const DeckState& state, const AppConfig& config, int radius, bool isFocused) {
    if (!btn.uiTree.is_object() || btn.widgetId.empty()) return false;
    const auto cell = getCellRect(0, 0, 1, 1, state, config);
    WidgetTree tree{*this, btn, state, config, std::max(1, std::min(rect.w, rect.h)),
                    std::max(1, std::min(cell.w, cell.h)) / 120.0, SDL_GetTicks64(), radius};
    const auto layout = tree.layout(btn.uiTree, rect);
    if (!layout.supported || !layout.node) return false;
    // Localized values and version-2 icons now use the server's own layout.
    if (btn.widgetType == "Clock" && btn.clockIsAnalog &&
        !tree.hasVisible(layout, [&](const Json& node, SDL_Rect box) {
            return jsonStr(node, "type") == "macrodeck.clock-dial" && std::min(box.w, box.h) >= std::min(rect.w, rect.h) / 3;
        })) return false;
    if (btn.widgetType == "ActionButton" && !btn.labelText.empty() &&
        !tree.hasVisible(layout, [](const Json& node, SDL_Rect box) {
            return jsonStr(node, "type") == "ui.text" && box.h >= 9 && !widgetNodeText(node, object(node, "properties")).empty();
        })) return false;

    const auto& props = object(*layout.node, "properties");
    const auto rootBg = jsonStr(object(props, "modifiers"), "background",
                               jsonStr(props, "background", jsonStr(props, "backgroundColor")));
    if (rootBg != "transparent" && rootBg != "none") {
        const SDL_Color background = btn.hasBgColor ? SDL_Color{btn.bgR, btn.bgG, btn.bgB, 255}
            : (config.theme == 1 ? SDL_Color{255, 255, 255, 255} : SDL_Color{26, 26, 26, 255});
        drawRoundedBox(renderer_, rect, radius, background);
    }
    SDL_RenderSetClipRect(renderer_, &rect);
    tree.paint(layout, 1, true);
    maskRoundedCorners(renderer_, rect, radius, currentCanvasBg_);
    SDL_RenderSetClipRect(renderer_, nullptr);
    const bool rootButton = jsonStr(*layout.node, "type") == "ui.button";
    if (!rootButton && !btn.borderStyle.empty()) tree.buttonRing(emptyObject, rect, radius, 1);
    if (btn.hasBorder && !btn.borderExplicitlyOff && btn.borderStyle.empty() && !props.contains("borderStyle")) {
        drawRoundedBorder(renderer_, rect, radius, 2, {btn.borderR, btn.borderG, btn.borderB, 255});
    }
    if (!rootButton && btn.widgetType == "ActionButton" && btn.visualPressFraction > 0) {
        drawRoundedBox(renderer_, rect, radius, {255, 255, 255, static_cast<Uint8>(51 * btn.visualPressFraction)});
    }
    if (btn.widgetType == "Slider" && btn.sliderShowValue && !tree.hasVisible(layout, [&](const Json& node, SDL_Rect box) {
            const auto type = jsonStr(node, "type");
            return textComponent(type) && box.h >= 9 &&
                   tree.label(node, object(node, "properties"), type).find('%') != std::string::npos;
        })) {
        const auto value = std::to_string(static_cast<int>(std::lround(std::clamp(btn.sliderLevel, 0.0f, 1.0f) * 100))) + "%";
        const auto color = config.theme == 1 ? SDL_Color{22, 25, 30, 255} : SDL_Color{245, 245, 245, 255};
        const bool vertical = btn.sliderIsVertical;
        const int size = std::clamp(std::min(rect.w, rect.h) / 7, 9, 18);
        drawText(widgetFont(size, true), value,
                 vertical ? rect.x + rect.w / 2 : rect.x + rect.w - 8,
                 vertical ? rect.y + rect.h - size / 2 - 3 : rect.y + size / 2 + 5,
                 color, vertical, rect.w - 16, !vertical, true, true);
    }
    if (isFocused) drawRoundedBorder(renderer_, rect, radius, 2, {49, 180, 255, 255});
    return true;
}

std::optional<std::string> UIRenderer::getWidgetTreeButtonAt(int x, int y, const DeckButton& btn,
                                                           const DeckState& state, const AppConfig& config) {
    const auto rect = getCellRect(btn.posY, btn.posX, btn.width, btn.height, state, config);
    const SDL_Point point{x, y};
    const bool idleSpotifyTap = SDL_PointInRect(&point, &rect) && state.findSpotifyPlayAction(btn);
    if (!btn.uiTree.is_object()) {
        if (idleSpotifyTap) return "switchdeck:spotify-play";
        return std::nullopt;
    }
    const auto cell = getCellRect(0, 0, 1, 1, state, config);
    WidgetTree tree{*this, btn, state, config, std::max(1, std::min(rect.w, rect.h)),
                    std::max(1, std::min(cell.w, cell.h)) / 120.0, SDL_GetTicks64()};
    const auto layout = tree.layout(btn.uiTree, rect);
    if (!layout.supported) {
        if (idleSpotifyTap) return "switchdeck:spotify-play";
        return std::nullopt;
    }
    auto nodeId = tree.buttonAt(layout, point);
    if (nodeId.empty() && idleSpotifyTap) return "switchdeck:spotify-play";
    return nodeId;
}

float UIRenderer::LevelControl::snap(float value) const {
    return std::clamp(step > 0 ? std::round(value / step) * step : value, 0.0f, 1.0f);
}

float UIRenderer::LevelControl::valueAt(SDL_Point pointer, SDL_Point previous, float previousLevel, bool beginning) const {
    if (dial) {
        const double x = area.x + area.w * 0.5;
        const double y = area.y + area.h * 0.5;
        const double radius = std::min(area.w, area.h) * 0.5;
        if (std::abs(sweep) < 0.01 || std::hypot(pointer.x - x, pointer.y - y) < radius * 0.2) return previousLevel;
        const auto angleAt = [&](SDL_Point point) { return std::atan2(point.x - x, y - point.y) / radians; };
        const double angle = angleAt(pointer);
        if (!beginning) {
            if (std::hypot(previous.x - x, previous.y - y) < radius * 0.2) return previousLevel;
            return std::clamp(previousLevel + static_cast<float>(std::remainder(angle - angleAt(previous), 360) / sweep), 0.0f, 1.0f);
        }
        const double direction = sweep > 0 ? 1 : -1;
        const double offset = std::fmod((angle - startAngle) * direction + 720, 360);
        const double arc = std::abs(sweep);
        if (offset <= arc) return static_cast<float>(offset / arc);
        return offset - arc < 360 - offset ? 1.0f : 0.0f;
    }
    const int span = std::max(1, vertical ? area.h : area.w);
    if (relative) {
        if (beginning) return previousLevel;
        const int delta = vertical ? previous.y - pointer.y : pointer.x - previous.x;
        return std::clamp(previousLevel + static_cast<float>(delta) / span, 0.0f, 1.0f);
    }
    const int position = vertical ? area.y + area.h - pointer.y : pointer.x - area.x;
    return std::clamp(static_cast<float>(position) / span, 0.0f, 1.0f);
}

std::optional<UIRenderer::LevelControl> UIRenderer::getTreeLevelControlAt(
    int x, int y, const DeckButton& btn, const DeckState& state, const AppConfig& config) {
    if (!btn.uiTree.is_object() || btn.widgetId.empty()) return std::nullopt;
    const auto rect = getCellRect(btn.posY, btn.posX, btn.width, btn.height, state, config);
    const auto cell = getCellRect(0, 0, 1, 1, state, config);
    WidgetTree tree{*this, btn, state, config, std::max(1, std::min(rect.w, rect.h)),
                    std::max(1, std::min(cell.w, cell.h)) / 120.0, SDL_GetTicks64()};
    const auto layout = tree.layout(btn.uiTree, rect);
    if (!layout.supported) return std::nullopt;
    if (auto hit = tree.levelAt(layout, {x, y})) return hit;
    return LevelControl{};
}

void UIRenderer::renderWidgetBorder(const DeckButton& btn, const SDL_Rect& rect, int radius,
                                    const DeckState& state, const AppConfig& config) {
    const auto cell = getCellRect(0, 0, 1, 1, state, config);
    WidgetTree tree{*this, btn, state, config, std::max(1, std::min(rect.w, rect.h)),
                    std::max(1, std::min(cell.w, cell.h)) / 120.0, SDL_GetTicks64(), radius};
    tree.buttonRing(emptyObject, rect, radius, 1);
}
