#include "deck_state.hpp"
#include "base64.hpp"
#include "widget_localization.hpp"
#include <SDL2/SDL_image.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <set>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <webp/demux.h>

static std::string trimText(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' ' || s.front() == '\t')) {
        s.erase(0, 1);
    }
    return s;
}

void DeckButton::cleanupTextures() {
    // Legacy buttons own their icons. Macro Deck 3 buttons borrow cached textures.
    if (widgetId.empty() && iconTexture) SDL_DestroyTexture(iconTexture);
    iconTexture = nullptr;
    if (labelTexture) {
        SDL_DestroyTexture(labelTexture);
        labelTexture = nullptr;
    }
    loadedResourceId.clear();
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

void DeckButton::updateAnimations(Uint64 nowTicks, float deltaSec) {
    if (deltaSec <= 0.0f) deltaSec = 0.016f;
    if (deltaSec > 0.1f) deltaSec = 0.1f;

    const float held = std::min(deltaSec, pressFeedbackSeconds);
    pressFeedbackSeconds -= held;
    const float press = isPressed ? deltaSec : held;
    visualPressFraction = std::min(1.0f, visualPressFraction + press / 0.020f);
    if (!isPressed) {
        visualPressFraction = std::max(0.0f, visualPressFraction - (deltaSec - held) / 0.140f);
    }

    // Macro Deck 3 slider transition:
    // When actively sliding or recently released, track instantaneous touch without lag
    if (visualSliderLevel < 0.0f) {
        visualSliderLevel = sliderLevel;
    } else {
        if (lastLocalSlideTick > 0 && (nowTicks >= lastLocalSlideTick) && (nowTicks - lastLocalSlideTick < 350)) {
            visualSliderLevel = sliderLevel;
        } else {
            float diff = sliderLevel - visualSliderLevel;
            if (std::fabs(diff) < 0.002f) {
                visualSliderLevel = sliderLevel;
            } else {
                float maxStep = deltaSec / 0.050f;
                if (diff > 0.0f) {
                    visualSliderLevel = std::min(sliderLevel, visualSliderLevel + maxStep);
                } else {
                    visualSliderLevel = std::max(sliderLevel, visualSliderLevel - maxStep);
                }
            }
        }
    }
    lastAnimUpdateTick = nowTicks;
}

DeckState::DeckState() {
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);
}

DeckState::~DeckState() {
    reset(nullptr);
    IMG_Quit();
}

void DeckState::reset(SDL_Renderer* renderer) {
    (void)renderer;
    for (auto& b : buttons_) {
        b.cleanupTextures();
    }
    buttons_.clear();
    for (auto& f : folders_) {
        for (auto& w : f.widgets) {
            w.cleanupTextures();
        }
    }
    folders_.clear();
    folderHistory_.clear();
    sessionToWidget_.clear();
    currentFolderId_.clear();

    for (auto& kv : resourceCache_) {
        if (kv.second.texture && kv.second.frames.empty()) {
            SDL_DestroyTexture(kv.second.texture);
            kv.second.texture = nullptr;
        }
    }
    resourceCache_.clear();
    resourceCacheBytes_ = 0;
    nextResourceOrder_ = 0;
    profiles_.clear();
    currentProfileId_.clear();
    currentProfileName_ = "Default";
}

static bool parseHexColor(const std::string& hexStr, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (hexStr.empty()) return false;
    std::string s = hexStr;
    if (s[0] == '#') s = s.substr(1);
    if (s.size() == 3) {
        std::string expanded;
        expanded.push_back(s[0]); expanded.push_back(s[0]);
        expanded.push_back(s[1]); expanded.push_back(s[1]);
        expanded.push_back(s[2]); expanded.push_back(s[2]);
        s = expanded;
    }
    if (s.size() == 6 || s.size() == 8) {
        try {
            r = static_cast<uint8_t>(std::stoul(s.substr(0, 2), nullptr, 16));
            g = static_cast<uint8_t>(std::stoul(s.substr(2, 2), nullptr, 16));
            b = static_cast<uint8_t>(std::stoul(s.substr(4, 2), nullptr, 16));
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

static void applyRoundedCornersToSurface(SDL_Surface* surf, float radiusRatio) {
    if (!surf || surf->w <= 0 || surf->h <= 0) return;
    if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);

    int w = surf->w;
    int h = surf->h;
    float radius = std::min(w, h) * radiusRatio;
    if (radius < 2.0f) {
        if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
        return;
    }

    int rCeil = static_cast<int>(std::ceil(radius)) + 1;
    Uint32* pixels = static_cast<Uint32*>(surf->pixels);
    int pitchPixels = surf->pitch / 4;

    auto processPixel = [&](int x, int y, float cx, float cy) {
        float dx = (x + 0.5f) - cx;
        float dy = (y + 0.5f) - cy;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > radius - 0.5f) {
            float alphaFactor = 1.0f - (dist - (radius - 0.5f));
            if (alphaFactor < 0.0f) alphaFactor = 0.0f;
            if (alphaFactor > 1.0f) alphaFactor = 1.0f;
            Uint32 px = pixels[y * pitchPixels + x];
            Uint8 a = static_cast<Uint8>((px >> 24) & 0xFF);
            Uint8 newA = static_cast<Uint8>(a * alphaFactor);
            pixels[y * pitchPixels + x] = (px & 0x00FFFFFFu) | (static_cast<Uint32>(newA) << 24);
        }
    };

    for (int y = 0; y < std::min(rCeil, h); ++y) {
        for (int x = 0; x < std::min(rCeil, w); ++x) {
            processPixel(x, y, radius, radius);
        }
        for (int x = std::max(0, w - rCeil); x < w; ++x) {
            processPixel(x, y, w - radius, radius);
        }
    }
    for (int y = std::max(0, h - rCeil); y < h; ++y) {
        for (int x = 0; x < std::min(rCeil, w); ++x) {
            processPixel(x, y, radius, h - radius);
        }
        for (int x = std::max(0, w - rCeil); x < w; ++x) {
            processPixel(x, y, w - radius, h - radius);
        }
    }

    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
}

SDL_Texture* DeckState::createTextureFromBytes(const uint8_t* data, size_t len, SDL_Renderer* renderer, int& outW, int& outH, bool roundCorners) {
    if (!data || len == 0 || !renderer) return nullptr;

    // Preprocess SVG payloads so nanosvg renders currentColor as white (#ffffff) and rasterizes at crisp 256x256
    std::string svgModified;
    const uint8_t* actualData = data;
    size_t actualLen = len;
    if (len < 262144) {
        std::string_view sv(reinterpret_cast<const char*>(data), len);
        size_t svgPos = sv.find("<svg");
        if (svgPos != std::string_view::npos) {
            svgModified.assign(reinterpret_cast<const char*>(data), len);
            size_t pos = 0;
            while ((pos = svgModified.find("currentColor", pos)) != std::string::npos) {
                svgModified.replace(pos, 12, "#ffffff");
                pos += 7;
            }
            size_t tagStart = svgModified.find("<svg");
            size_t tagEnd = (tagStart != std::string::npos) ? svgModified.find('>', tagStart) : std::string::npos;
            if (tagStart != std::string::npos && tagEnd != std::string::npos) {
                std::string openTag = svgModified.substr(tagStart, tagEnd - tagStart + 1);
                bool hasViewBox = (openTag.find("viewBox=") != std::string::npos);
                auto extractAttr = [&](const char* attrName) -> std::string {
                    size_t ap = openTag.find(attrName);
                    if (ap == std::string::npos) return "";
                    size_t q1 = openTag.find_first_of("\"'", ap);
                    if (q1 == std::string::npos) return "";
                    char qc = openTag[q1];
                    size_t q2 = openTag.find(qc, q1 + 1);
                    if (q2 == std::string::npos) return "";
                    return openTag.substr(q1 + 1, q2 - q1 - 1);
                };
                std::string origW = extractAttr("width=");
                std::string origH = extractAttr("height=");
                auto stripAttr = [&](std::string& tag, const char* attrName) {
                    size_t ap = tag.find(attrName);
                    if (ap == std::string::npos) return;
                    size_t q1 = tag.find_first_of("\"'", ap);
                    if (q1 == std::string::npos) return;
                    char qc = tag[q1];
                    size_t q2 = tag.find(qc, q1 + 1);
                    if (q2 == std::string::npos) return;
                    tag.erase(ap, q2 - ap + 1);
                };
                stripAttr(openTag, "width=");
                stripAttr(openTag, "height=");
                std::string inject = " width=\"256\" height=\"256\"";
                if (!hasViewBox) {
                    std::string vw = origW.empty() ? "24" : origW;
                    std::string vh = origH.empty() ? "24" : origH;
                    inject += " viewBox=\"0 0 " + vw + " " + vh + "\"";
                }
                openTag.insert(4, inject);
                svgModified.replace(tagStart, tagEnd - tagStart + 1, openTag);
            }
            actualData = reinterpret_cast<const uint8_t*>(svgModified.data());
            actualLen = svgModified.size();
        }
    }

    SDL_RWops* rw = SDL_RWFromConstMem(actualData, static_cast<int>(actualLen));
    if (!rw) return nullptr;

    SDL_Surface* surf = IMG_Load_RW(rw, 1);
    if (!surf) {
        return nullptr;
    }

    if (roundCorners) {
        SDL_Surface* argbSurf = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ARGB8888, 0);
        SDL_FreeSurface(surf);
        if (!argbSurf) return nullptr;
        surf = argbSurf;
        applyRoundedCornersToSurface(surf, 0.14f);
    }

    outW = surf->w;
    outH = surf->h;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    SDL_FreeSurface(surf);
    return tex;
}

SDL_Texture* DeckState::createTextureFromBase64(const std::string& b64, SDL_Renderer* renderer, int& outW, int& outH, bool roundCorners) {
    if (b64.empty() || !renderer) return nullptr;
    std::vector<uint8_t> raw = base64_decode(b64);
    if (raw.empty()) return nullptr;
    return createTextureFromBytes(raw.data(), raw.size(), renderer, outW, outH, roundCorners);
}

// ============================================================================
// Macro Deck 2 Legacy Handlers
// ============================================================================

void DeckState::handleConfig(const nlohmann::json& j) {
    if (!folders_.empty()) return; // Never let legacy MD2 GET_CONFIG overwrite modern MD3 folder grid
    rows_ = jsonInt(j, "Rows", 3);
    cols_ = jsonInt(j, "Columns", 6);
    spacing_ = jsonInt(j, "ButtonSpacing", 16);
    radius_ = jsonInt(j, "ButtonRadius", 28);
    if (rows_ < 1) rows_ = 3;
    if (cols_ < 1) cols_ = 6;
}

void DeckState::handleButtons(const nlohmann::json& j, SDL_Renderer* renderer) {
    if (!j.contains("Buttons") || !j["Buttons"].is_array()) return;

    for (auto& b : buttons_) {
        b.cleanupTextures();
    }
    buttons_.clear();

    for (const auto& bJson : j["Buttons"]) {
        updateSingleButton(bJson, renderer);
    }
}

void DeckState::handleUpdateButton(const nlohmann::json& j, SDL_Renderer* renderer) {
    if (j.contains("Buttons") && j["Buttons"].is_array()) {
        for (const auto& item : j["Buttons"]) {
            updateSingleButton(item, renderer);
        }
    } else {
        updateSingleButton(j, renderer);
    }
}

void DeckState::handleUpdateLabel(const nlohmann::json& j, SDL_Renderer* renderer) {
    handleUpdateButton(j, renderer);
}

void DeckState::updateSingleButton(const nlohmann::json& bJson, SDL_Renderer* renderer) {
    int x = jsonInt(bJson, "Position_X", -1);
    int y = jsonInt(bJson, "Position_Y", -1);
    if (x < 0 || y < 0) return;

    DeckButton* target = getButton(y, x);
    if (!target) {
        DeckButton newBtn;
        newBtn.posX = x;
        newBtn.posY = y;
        buttons_.push_back(newBtn);
        target = &buttons_.back();
    }

    std::string hex = jsonStr(bJson, "BackgroundColorHex", jsonStr(bJson, "BackColorHex", ""));
    if (!hex.empty()) {
        target->hasBgColor = parseHexColor(hex, target->bgR, target->bgG, target->bgB);
    }

    std::string lbl = trimText(jsonStr(bJson, "Label", ""));
    if (!lbl.empty()) {
        target->labelText = lbl;
    }

    if (bJson.contains("IconBase64")) {
        std::string newIcon = jsonStr(bJson, "IconBase64", "");
        if (newIcon != target->iconBase64) {
            target->iconBase64 = newIcon;
            if (target->iconTexture) {
                SDL_DestroyTexture(target->iconTexture);
                target->iconTexture = nullptr;
            }
            target->iconTexture = createTextureFromBase64(target->iconBase64, renderer, target->iconWidth, target->iconHeight);
        }
    }

    if (bJson.contains("LabelBase64")) {
        std::string newLbl = jsonStr(bJson, "LabelBase64", "");
        if (newLbl != target->labelBase64) {
            target->labelBase64 = newLbl;
            if (target->labelTexture) {
                SDL_DestroyTexture(target->labelTexture);
                target->labelTexture = nullptr;
            }
            target->labelTexture = createTextureFromBase64(target->labelBase64, renderer, target->labelWidth, target->labelHeight);
        }
    }
}

// ============================================================================
// Macro Deck 3 Profile / Folder / Widget Parsing
// ============================================================================

std::string DeckState::handleMd3Profiles(const nlohmann::json& j, const std::string& preferredProfileId) {
    const nlohmann::json* arr = nullptr;
    if (j.is_array()) {
        arr = &j;
    } else if (j.is_object() && j.contains("profiles") && j["profiles"].is_array()) {
        arr = &j["profiles"];
    } else if (j.is_object() && j.contains("items") && j["items"].is_array()) {
        arr = &j["items"];
    }

    if (!arr) return currentProfileId_;
    if (arr->empty()) {
        profiles_.clear();
        currentProfileId_.clear();
        currentProfileName_ = "Default";
        return {};
    }

    profiles_.clear();
    for (const auto& p : *arr) {
        if (!p.is_object()) continue;
        ProfileInfo pi;
        pi.id = jsonStr(p, "id", "");
        pi.name = jsonStr(p, "name", "Profile");
        pi.isDefault = jsonBool(p, "isDefault", false);
        if (!pi.id.empty()) {
            profiles_.push_back(std::move(pi));
        }
    }

    const nlohmann::json* chosen = &((*arr)[0]);
    for (const auto& p : *arr) {
        if (!p.is_object()) continue;
        std::string id = jsonStr(p, "id", "");
        bool isDef = jsonBool(p, "isDefault", false);
        if (!preferredProfileId.empty() && id == preferredProfileId) {
            chosen = &p;
            break;
        }
        if (preferredProfileId.empty() && isDef) {
            chosen = &p;
        }
    }

    currentProfileId_ = jsonStr(*chosen, "id", "");
    currentProfileName_ = jsonStr(*chosen, "name", "Main");
    const auto layout = chosen->value("layout", nlohmann::json::object());
    profileDefaultRows_ = jsonInt(*chosen, "defaultRows", jsonInt(layout, "rows", jsonInt(*chosen, "rows", 3)));
    profileDefaultCols_ = jsonInt(*chosen, "defaultColumns", jsonInt(layout, "columns", jsonInt(*chosen, "columns", 6)));
    profileDefaultSpacing_ = jsonInt(*chosen, "defaultWidgetSpacing", jsonInt(*chosen, "widgetSpacing", profileDefaultSpacing_));
    profileDefaultRadius_ = jsonInt(*chosen, "defaultWidgetBorderRadius", jsonInt(*chosen, "widgetBorderRadius", profileDefaultRadius_));

    std::string emptyStyle = jsonStr(*chosen, "defaultEmptyCellStyle", "");
    if (!emptyStyle.empty()) {
        emptyCellsVisible_ = (emptyStyle != "hidden" && emptyStyle != "off");
    } else {
        emptyCellsVisible_ = jsonBool(*chosen, "defaultEmptyCellsVisible", jsonBool(*chosen, "emptyCellsVisible", true));
    }

    if (profileDefaultRows_ <= 0) profileDefaultRows_ = 3;
    if (profileDefaultCols_ <= 0) profileDefaultCols_ = 6;
    if (profileDefaultSpacing_ < 0) profileDefaultSpacing_ = 12;
    if (profileDefaultRadius_ < 0) profileDefaultRadius_ = 28;

    // Only apply profile defaults directly if no folders are loaded yet
    if (folders_.empty()) {
        rows_ = profileDefaultRows_;
        cols_ = profileDefaultCols_;
        spacing_ = profileDefaultSpacing_;
        radius_ = profileDefaultRadius_;
    }

    return currentProfileId_;
}

void DeckState::handleMd3ProfileUpdate(const nlohmann::json& pJson, SDL_Renderer* renderer) {
    const nlohmann::json* p = &pJson;
    if (pJson.contains("profile") && pJson["profile"].is_object()) {
        p = &pJson["profile"];
    }
    std::string id = jsonStr(*p, "id", "");
    if (!id.empty() && !currentProfileId_.empty() && id != currentProfileId_) {
        return;
    }
    std::string name = jsonStr(*p, "name", "");
    if (!name.empty()) currentProfileName_ = name;

    int newRows = jsonInt(*p, "defaultRows", jsonInt(*p, "rows", -1));
    int newCols = jsonInt(*p, "defaultColumns", jsonInt(*p, "columns", -1));
    int newSpacing = jsonInt(*p, "defaultWidgetSpacing", jsonInt(*p, "widgetSpacing", -1));
    int newRadius = jsonInt(*p, "defaultWidgetBorderRadius", jsonInt(*p, "widgetBorderRadius", -1));

    if (newRows > 0) profileDefaultRows_ = newRows;
    if (newCols > 0) profileDefaultCols_ = newCols;
    if (newSpacing >= 0) profileDefaultSpacing_ = newSpacing;
    if (newRadius >= 0) profileDefaultRadius_ = newRadius;

    std::string emptyStyle = jsonStr(*p, "defaultEmptyCellStyle", "");
    if (!emptyStyle.empty()) {
        emptyCellsVisible_ = (emptyStyle != "hidden" && emptyStyle != "off");
    }

    syncActiveFolderToButtons(renderer);
}

static void applyButtonBorder(DeckButton& button, const nlohmann::json& props) {
    const auto found = props.find("border");
    const bool nested = found != props.end() && found->is_object();
    const auto& border = nested ? *found : props;
    auto style = jsonStr(border, nested ? "style" : "borderStyle");
    if (nested && style.empty() && border.contains("enabled")) {
        style = jsonBool(border, "enabled") ? "static" : "off";
    }
    if (props.contains("border") && found->is_boolean() && !found->get<bool>()) style = "off";
    if (!style.empty()) {
        for (auto& ch : style) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (style == "hueshift" || style == "hue_shift") style = "hue-shift";
        if (style == "heart-beat") style = "heartbeat";
        if (style == "solid") style = "static";
        button.borderStyle = style;
        button.borderExplicitlyOff = style == "off" || style == "none";
        button.hasBorder = !button.borderExplicitlyOff;
    }
    const auto color = jsonStr(border, nested ? "color" : "borderColor");
    if (!button.borderExplicitlyOff && parseHexColor(color, button.borderR, button.borderG, button.borderB)) {
        button.hasBorder = true;
    }
}

DeckButton DeckState::parseMd3Widget(const nlohmann::json& wJson, const std::string& folderId) {
    DeckButton btn;
    btn.widgetId = jsonStr(wJson, "id", "");
    btn.folderId = folderId;
    btn.widgetType = jsonStr(wJson, "type", jsonStr(wJson, "widgetType", "ActionButton"));
    {
        std::string wtLower = btn.widgetType;
        for (auto& ch : wtLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (wtLower == "calendar" || wtLower.find("::calendar") != std::string::npos) btn.widgetType = "Calendar";
        else if (wtLower == "musicplayer" || wtLower.find("::music") != std::string::npos) btn.widgetType = "MusicPlayer";
        else if (wtLower == "slider" || wtLower.find("::slider") != std::string::npos) btn.widgetType = "Slider";
        else if (wtLower == "weather" || wtLower.find("::weather") != std::string::npos) btn.widgetType = "Weather";
        else if (wtLower == "gauges" || wtLower.find("::gauges") != std::string::npos) btn.widgetType = "Gauges";
        else if (wtLower == "countdown" || wtLower.find("::countdown") != std::string::npos) btn.widgetType = "Countdown";
        else if (wtLower == "stopwatch" || wtLower.find("::stopwatch") != std::string::npos) btn.widgetType = "Stopwatch";
        else if (wtLower == "battery" || wtLower.find("::battery") != std::string::npos) btn.widgetType = "Battery";
        else if (wtLower == "historygraph" || wtLower.find("history-graph") != std::string::npos || wtLower.find("historygraph") != std::string::npos) btn.widgetType = "HistoryGraph";
        else if (wtLower == "clock" || wtLower.find("::clock") != std::string::npos) btn.widgetType = "Clock";
        else if (wtLower == "actionbutton" || wtLower.find("::action") != std::string::npos) btn.widgetType = "ActionButton";
    }

    if (wJson.contains("positionX") || wJson.contains("positionY")) {
        btn.posX = jsonInt(wJson, "positionX", 0);
        btn.posY = jsonInt(wJson, "positionY", 0);
        btn.width = std::max(1, jsonInt(wJson, "width", 1));
        btn.height = std::max(1, jsonInt(wJson, "height", 1));
    } else if (wJson.contains("gridArea") && wJson["gridArea"].is_object()) {
        const auto& ga = wJson["gridArea"];
        int rowStart = jsonInt(ga, "rowStart", 1);
        int colStart = jsonInt(ga, "columnStart", 1);
        btn.posY = std::max(0, rowStart - 1);
        btn.posX = std::max(0, colStart - 1);
        btn.height = std::max(1, jsonInt(ga, "rowSpan", 1));
        btn.width = std::max(1, jsonInt(ga, "columnSpan", 1));
    } else {
        btn.posX = jsonInt(wJson, "column", jsonInt(wJson, "x", 0));
        btn.posY = jsonInt(wJson, "row", jsonInt(wJson, "y", 0));
        btn.width = std::max(1, jsonInt(wJson, "columnSpan", jsonInt(wJson, "width", 1)));
        btn.height = std::max(1, jsonInt(wJson, "rowSpan", jsonInt(wJson, "height", 1)));
    }

    btn.bgR = 26; btn.bgG = 26; btn.bgB = 26;
    btn.hasBgColor = false;
    btn.accentR = 33; btn.accentG = 150; btn.accentB = 243;
    btn.hasAccentColor = false;

    if (btn.widgetType == "MusicPlayer") {
        btn.bgR = 24; btn.bgG = 34; btn.bgB = 30;
        btn.accentR = 29; btn.accentG = 185; btn.accentB = 84;
        btn.hasAccentColor = true;
        btn.hasProgressBar = true;
    } else if (btn.widgetType == "Slider") {
        btn.hasProgressBar = true;
    }

    nlohmann::json data;
    if (wJson.contains("data")) {
        if (wJson["data"].is_string()) {
            try {
                data = nlohmann::json::parse(wJson["data"].get<std::string>());
            } catch (...) {}
        } else if (wJson["data"].is_object()) {
            data = wJson["data"];
        }
    }

    if (data.is_object()) {
        std::string lbl = trimText(jsonStr(data, "label", jsonStr(data, "title", "")));
        if (lbl.find("şuna değiştir") != std::string::npos || lbl.find("Change folder") != std::string::npos) {
            lbl.clear();
        }
        if (!lbl.empty()) {
            btn.staticLabel = lbl;
        }
        btn.labelPosition = jsonStr(data, "labelPosition", "center");
        btn.textAlign = jsonStr(data, "textAlign", "center");
        btn.fontSize = jsonInt(data, "fontSize", 14);
        btn.labelShadow = jsonBool(data, "labelShadow", true);

        // Background color
        std::string bgHex = jsonStr(data, "backgroundColor", jsonStr(data, "background", ""));
        if (parseHexColor(bgHex, btn.bgR, btn.bgG, btn.bgB)) {
            btn.hasBgColor = true;
        }

        // Label / Text color
        std::string lblColHex = jsonStr(data, "labelColor", jsonStr(data, "textColor", ""));
        if (parseHexColor(lblColHex, btn.textR, btn.textG, btn.textB)) {
            btn.hasTextColor = true;
        }

        // Accent / fill color
        std::string accentHex = jsonStr(data, "color", jsonStr(data, "accentColor", ""));
        if (parseHexColor(accentHex, btn.accentR, btn.accentG, btn.accentB)) {
            btn.hasAccentColor = true;
        }

        applyButtonBorder(btn, data);

        // Static icon & iconDisplay settings
        if (data.contains("icon")) {
            if (data["icon"].is_object()) {
                const auto& iconObj = data["icon"];
                std::string rid = jsonStr(iconObj, "resourceId", "");
                if (rid.empty()) rid = jsonStr(iconObj, "reference", "");
                if (!rid.empty()) btn.resourceId = rid;
                std::string fit = jsonStr(iconObj, "fit", "");
                if (!fit.empty()) btn.iconFit = fit;
                std::string ibytes = jsonStr(iconObj, "iconBytes", jsonStr(iconObj, "iconBase64", ""));
                if (!ibytes.empty()) {
                    btn.iconBase64 = ibytes;
                }
            } else if (data["icon"].is_string()) {
                std::string s = data["icon"].get<std::string>();
                if (s.rfind("data:image", 0) == 0 || s.size() > 100) {
                    btn.iconBase64 = s;
                } else if (!s.empty()) {
                    btn.resourceId = s;
                }
            }
        }
        if (btn.iconBase64.empty()) {
            std::string ibytes = jsonStr(data, "iconBytes", jsonStr(data, "iconBase64", ""));
            if (!ibytes.empty()) btn.iconBase64 = ibytes;
        }
        if (btn.resourceId.empty()) {
            std::string icId = jsonStr(data, "iconId", "");
            if (!icId.empty()) btn.resourceId = icId;
        }

        if (data.contains("iconDisplay") && data["iconDisplay"].is_object()) {
            const auto& idisp = data["iconDisplay"];
            std::string fit = jsonStr(idisp, "fit", "");
            if (!fit.empty()) btn.iconFit = fit;
            btn.iconZoom = std::clamp(jsonInt(idisp, "zoom", 100), 25, 250);
            btn.iconOffsetX = std::clamp(jsonInt(idisp, "offsetX", 0), -50, 50);
            btn.iconOffsetY = std::clamp(jsonInt(idisp, "offsetY", 0), -50, 50);
            btn.iconOpacity = std::clamp(jsonInt(idisp, "opacity", 100), 5, 100);
        }

        // ActionButton State Mode (stateMode: true, activeStateId: "on", states: [...])
        if (jsonBool(data, "stateMode", false) && data.contains("states") && data["states"].is_array() && !data["states"].empty()) {
            std::string activeId = jsonStr(data, "activeStateId", "");
            const nlohmann::json* activeState = &data["states"][0];
            for (const auto& st : data["states"]) {
                if (st.is_object() && jsonStr(st, "id") == activeId) {
                    activeState = &st;
                    break;
                }
            }
            if (activeState && activeState->is_object()) {
                const nlohmann::json& app = (activeState->contains("appearance") && (*activeState)["appearance"].is_object())
                                            ? (*activeState)["appearance"] : *activeState;
                applyButtonBorder(btn, app);
                std::string stLabel = trimText(jsonStr(app, "label", ""));
                if (!stLabel.empty()) {
                    btn.staticLabel = stLabel;
                }
                std::string stBg = jsonStr(app, "backgroundColor", jsonStr(app, "background", ""));
                if (parseHexColor(stBg, btn.bgR, btn.bgG, btn.bgB)) {
                    btn.hasBgColor = true;
                }
                std::string stTextCol = jsonStr(app, "labelColor", jsonStr(app, "textColor", ""));
                if (parseHexColor(stTextCol, btn.textR, btn.textG, btn.textB)) {
                    btn.hasTextColor = true;
                }
                if (app.contains("icon")) {
                    if (app["icon"].is_object()) {
                        const auto& appIcon = app["icon"];
                        std::string rid = jsonStr(appIcon, "resourceId", "");
                        if (rid.empty()) rid = jsonStr(appIcon, "reference", "");
                        if (!rid.empty()) btn.resourceId = rid;
                        std::string ibytes = jsonStr(appIcon, "iconBytes", jsonStr(appIcon, "iconBase64", ""));
                        if (!ibytes.empty()) btn.iconBase64 = ibytes;
                    } else if (app["icon"].is_string()) {
                        std::string s = app["icon"].get<std::string>();
                        if (s.rfind("data:image", 0) == 0 || s.size() > 100) {
                            btn.iconBase64 = s;
                        } else if (!s.empty()) {
                            btn.resourceId = s;
                        }
                    }
                }
                if (btn.iconBase64.empty()) {
                    std::string ibytes = jsonStr(app, "iconBytes", jsonStr(app, "iconBase64", ""));
                    if (!ibytes.empty()) btn.iconBase64 = ibytes;
                }
                if (btn.resourceId.empty()) {
                    std::string icId = jsonStr(app, "iconId", "");
                    if (!icId.empty()) btn.resourceId = icId;
                }
                if (app.contains("iconDisplay") && app["iconDisplay"].is_object()) {
                    const auto& idisp = app["iconDisplay"];
                    std::string fit = jsonStr(idisp, "fit", "");
                    if (!fit.empty()) btn.iconFit = fit;
                    btn.iconZoom = std::clamp(jsonInt(idisp, "zoom", btn.iconZoom), 25, 250);
                    btn.iconOffsetX = std::clamp(jsonInt(idisp, "offsetX", btn.iconOffsetX), -50, 50);
                    btn.iconOffsetY = std::clamp(jsonInt(idisp, "offsetY", btn.iconOffsetY), -50, 50);
                    btn.iconOpacity = std::clamp(jsonInt(idisp, "opacity", btn.iconOpacity), 5, 100);
                }
            }
        }

        // MusicPlayer specific static settings
        if (btn.widgetType == "MusicPlayer") {
            const nlohmann::json* mcfg = &data;
            if (data.contains("configuration") && data["configuration"].is_object()) mcfg = &data["configuration"];
            else if (data.contains("customConfig") && data["customConfig"].is_object()) mcfg = &data["customConfig"];
            else if (data.contains("properties") && data["properties"].is_object()) mcfg = &data["properties"];

            btn.musicInstanceId = jsonStr(*mcfg, "instanceId", jsonStr(data, "instanceId"));
            btn.musicCoverStyle = jsonStr(*mcfg, "coverStyle", jsonStr(data, "coverStyle", "small"));
            btn.musicShowHeader = jsonBool(*mcfg, "showHeader", jsonBool(data, "showHeader", true));
            btn.musicShowTitle = jsonBool(*mcfg, "showTitle", jsonBool(data, "showTitle", true));
            btn.musicShowArtist = jsonBool(*mcfg, "showArtist", jsonBool(data, "showArtist", true));
            btn.musicShowAlbum = jsonBool(*mcfg, "showAlbum", jsonBool(data, "showAlbum", true));
            btn.musicShowTimeline = jsonBool(*mcfg, "showTimeline", jsonBool(data, "showTimeline", true));
            btn.musicShowSource = jsonBool(*mcfg, "showSource", jsonBool(data, "showSource", true));
        }

        // Slider specific static settings
        if (btn.widgetType == "Slider") {
            std::string orient = jsonStr(data, "orientation", jsonStr(data, "direction", "horizontal"));
            btn.sliderIsVertical = (orient == "vertical" || orient == "Vertical");
            std::string inter = jsonStr(data, "interaction", "absolute");
            btn.sliderIsRelative = (inter == "relative" || inter == "Relative");
            btn.sliderShowLabel = jsonBool(data, "showLabel", true);
            btn.sliderShowValue = jsonBool(data, "showValue", false);
            btn.sliderMin = jsonDouble(data, "min", 0.0);
            btn.sliderMax = jsonDouble(data, "max", 100.0);
            btn.sliderStep = jsonDouble(data, "step", 1.0);
            btn.sliderValueVariable = jsonStr(data, "valueVariable", "");
            double curVal = jsonDouble(data, "value", btn.sliderMin);
            if (btn.sliderMax > btn.sliderMin) {
                btn.sliderLevel = static_cast<float>((curVal - btn.sliderMin) / (btn.sliderMax - btn.sliderMin));
                btn.sliderLevel = std::max(0.0f, std::min(1.0f, btn.sliderLevel));
            }
        }

        // Clock specific static settings
        if (btn.widgetType == "Clock") {
            std::string style = jsonStr(data, "style", jsonStr(data, "clockStyle", jsonStr(data, "face", "digital")));
            btn.clockIsAnalog = (style == "analog" || style == "Analog" || jsonBool(data, "isAnalog"));
            btn.clockConfiguredAnalog = btn.clockIsAnalog;
            btn.clockShowSeconds = jsonBool(data, "showSeconds", true);
            btn.clockShowDate = jsonBool(data, "showDate", true);
            btn.clockDatePosition = jsonStr(data, "datePosition", "below");
            btn.clockZoneText = jsonStr(data, "label", "");
        }

        // Weather specific static settings
        if (btn.widgetType == "Weather") {
            btn.weatherStyle = jsonStr(data, "style", "compact");
            btn.weatherLocation = jsonStr(data, "location", "");
        }

        // Gauges specific static settings
        if (btn.widgetType == "Gauges") {
            std::string style = jsonStr(data, "style", "bar");
            btn.gaugeIsArc = (style == "arc" || style == "Arc" || style == "ring" || style == "Ring");
            if (data.contains("gauges") && data["gauges"].is_array()) {
                for (const auto& gItem : data["gauges"]) {
                    if (!gItem.is_object()) continue;
                    GaugeItem gi;
                    gi.name = trimText(jsonStr(gItem, "name", jsonStr(gItem, "label", jsonStr(gItem, "variable", ""))));
                    gi.reading = "--";
                    std::string gCol = jsonStr(gItem, "color", "");
                    if (!gCol.empty() && parseHexColor(gCol, gi.r, gi.g, gi.b)) {
                        gi.hasColor = true;
                    }
                    if (gItem.contains("icon") && gItem["icon"].is_object()) {
                        std::string gRid = jsonStr(gItem["icon"], "resourceId", jsonStr(gItem["icon"], "reference", ""));
                        if (!gRid.empty()) {
                            gi.resourceId = gRid;
                            if (std::find(btn.treeResourceIds.begin(), btn.treeResourceIds.end(), gRid) == btn.treeResourceIds.end()) {
                                btn.treeResourceIds.push_back(gRid);
                            }
                        }
                    }
                    btn.gauges.push_back(std::move(gi));
                }
            }
        }

        // Countdown specific static settings
        if (btn.widgetType == "Countdown") {
            int sec = jsonInt(data, "durationHours", 0) * 3600 +
                      jsonInt(data, "durationMinutes", 0) * 60 +
                      jsonInt(data, "durationSeconds", 0);
            if (sec > 0) {
                btn.countdownConfiguredMs = btn.progressDurationMs = sec * 1000.0;
                btn.countdownTimeText = DeckButton::formatDurationMs(btn.progressDurationMs);
            }
            if (jsonBool(data, "showLabel", false)) {
                btn.countdownCaption = jsonStr(data, "label", "");
            } else {
                btn.countdownCaption.clear();
            }
        }

        // Battery specific static settings
        if (btn.widgetType == "Battery") {
            std::string bLbl = trimText(jsonStr(data, "label", ""));
            if (!bLbl.empty()) btn.labelText = bLbl;
        }

        // Stopwatch specific static settings
        if (btn.widgetType == "Stopwatch") {
            btn.stopwatchTimeText = "0:00";
        }

        // Calendar specific static settings
        if (btn.widgetType == "Calendar") {
            btn.calendarMonthText = "";
            btn.calendarDayText = "";
        }

        // HistoryGraph specific static settings
        if (btn.widgetType == "HistoryGraph") {
            btn.graphTitle = jsonStr(data, "title", "CPU Load");
            btn.graphUnit = jsonStr(data, "unit", "%");
        }

        if (!btn.resourceId.empty()) {
            if (std::find(btn.treeResourceIds.begin(), btn.treeResourceIds.end(), btn.resourceId) == btn.treeResourceIds.end()) {
                btn.treeResourceIds.push_back(btn.resourceId);
            }
        }

        // Parse flows (can be JSON string, array of trigger flows, or object)
        nlohmann::json flowsVal;
        if (data.contains("flows")) {
            if (data["flows"].is_string()) {
                try {
                    flowsVal = nlohmann::json::parse(data["flows"].get<std::string>());
                } catch (...) {}
            } else {
                flowsVal = data["flows"];
            }
        }

        if (flowsVal.is_array()) {
            for (const auto& flow : flowsVal) {
                if (!flow.is_object()) continue;
                std::string trig = jsonStr(flow, "triggerType", jsonStr(flow, "triggerId", ""));

                if (flow.contains("children") && flow["children"].is_array() && !flow["children"].empty()) {
                    if (trig == "onShortPress") btn.hasShortPressFlow = true;
                    else if (trig == "onDoublePress") btn.hasDoublePressFlow = true;
                    else if (trig == "onLongPress") btn.hasLongPressFlow = true;

                    const auto& firstBlock = flow["children"][0];
                    if (firstBlock.is_object()) {
                        std::string blockLabel = trimText(jsonStr(firstBlock, "label", ""));
                        std::string actionId = jsonStr(firstBlock, "actionId", "");
                        std::string blockType = jsonStr(firstBlock, "blockType", "");
                        if ((btn.widgetType == "ActionButton" || btn.widgetType == "MusicPlayer") && trig == "onShortPress" &&
                            flow["children"].size() == 1 && jsonStr(firstBlock, "type") == "action" &&
                            !jsonBool(firstBlock, "disabled") && jsonBool(firstBlock, "enabled", true) &&
                            (blockType == "com.cjhackeryt.spicetifydeck.spicetify-toggle-play" ||
                             (jsonStr(firstBlock, "integrationId") == "com.cjhackeryt.spicetifydeck" &&
                              actionId == "spicetify-toggle-play"))) {
                            btn.spotifyPlayAction = true;
                        }

                        if (actionId == "change-folder" || actionId == "switch-folder" ||
                            blockType.find("change-folder") != std::string::npos ||
                            blockType.find("switch-folder") != std::string::npos) {
                            btn.navCommand = "changeTo";
                            if (firstBlock.contains("parameters") && firstBlock["parameters"].is_array()) {
                                for (const auto& param : firstBlock["parameters"]) {
                                    if (param.is_object() && jsonStr(param, "name") == "folderId") {
                                        std::string fval = jsonStr(param, "value");
                                        if (!fval.empty()) btn.navTargetFolderId = fval;
                                    }
                                }
                            }
                        } else if (actionId == "parent-folder" || blockType.find("parent-folder") != std::string::npos) {
                            btn.navCommand = "parent";
                        } else if (actionId == "previous-folder" || actionId == "back-folder" || blockType.find("back") != std::string::npos) {
                            btn.navCommand = "back";
                        }
                    }
                }
            }
        } else if (flowsVal.is_object()) {
            auto inspectFlow = [&](const char* flowKey, bool& hasFlowFlag) {
                if (!flowsVal.contains(flowKey) || !flowsVal[flowKey].is_object()) return;
                const auto& fl = flowsVal[flowKey];
                if (!fl.contains("nodes") || !fl["nodes"].is_array() || fl["nodes"].empty()) return;
                for (const auto& node : fl["nodes"]) {
                    std::string nType = jsonStr(node, "type", "");
                    if (nType == "start") continue;
                    hasFlowFlag = true;
                    if (node.contains("config") && node["config"].is_object()) {
                        const auto& cfg = node["config"];
                        std::string cmd = jsonStr(cfg, "command", "");
                        if (cmd == "changeTo" || cmd == "parent" || cmd == "back") {
                            btn.navCommand = cmd;
                            btn.navTargetFolderId = jsonStr(cfg, "folderId", jsonStr(cfg, "targetFolderId", ""));
                        }
                    }
                }
            };
            inspectFlow("shortPress", btn.hasShortPressFlow);
            inspectFlow("doublePress", btn.hasDoublePressFlow);
            inspectFlow("longPress", btn.hasLongPressFlow);
        }
    }

    btn.labelText = btn.staticLabel;
    btn.subLabelText = btn.staticSubLabel;
    return btn;
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

void DeckState::handleMd3Folders(const nlohmann::json& j, const std::string& profileId,
                              bool profileChanged, SDL_Renderer* renderer) {
    const nlohmann::json* arr = nullptr;
    if (j.is_array()) {
        arr = &j;
    } else if (j.is_object() && j.contains("folders") && j["folders"].is_array()) {
        arr = &j["folders"];
    } else if (j.is_object() && j.contains("items") && j["items"].is_array()) {
        arr = &j["items"];
    }

    if (!arr) return;

    // Snapshot live runtime state of all widgets (both displayed buttons_ and folder widgets) before refreshing folders
    std::map<std::string, DeckButton> liveById;
    for (const auto& f : folders_) {
        for (const auto& w : f.widgets) {
            if (!w.widgetId.empty()) liveById[w.widgetId] = w;
        }
    }
    for (const auto& b : buttons_) {
        if (!b.widgetId.empty()) liveById[b.widgetId] = b;
    }

    std::string defaultFolderId;
    std::set<std::string> receivedFolders;
    for (const auto& fJson : *arr) {
        if (!fJson.is_object()) continue;
        std::string fid = jsonStr(fJson, "id", "");
        if (fid.empty()) continue;
        const auto owner = jsonStr(fJson, "profileId", profileId);
        if (!profileId.empty() && !owner.empty() && owner != profileId) continue;
        receivedFolders.insert(fid);

        DeckFolder* target = nullptr;
        for (auto& existing : folders_) {
            if (existing.id == fid) {
                target = &existing;
                break;
            }
        }
        if (!target) {
            folders_.emplace_back();
            target = &folders_.back();
            target->id = fid;
        }
        target->profileId = owner.empty() ? profileId : owner;

        std::string fn = jsonStr(fJson, "displayName", "");
        if (fn.empty()) fn = jsonStr(fJson, "name", "");
        if (fn.empty()) fn = jsonStr(fJson, "title", "");
        if (fn.empty()) fn = jsonStr(fJson, "folderName", "");
        if (fn.empty()) fn = target->name;
        if (fn.empty()) fn = target->isDefault ? "Home" : ("Sayfa " + std::to_string(folders_.size()));
        target->name = fn;
        target->parentId = jsonStr(fJson, "parentId", "");
        target->isDefault = jsonBool(fJson, "isDefault", target->isDefault);
        target->rows = jsonInt(fJson, "rows", -1);
        target->cols = jsonInt(fJson, "columns", -1);
        target->widgetSpacing = jsonInt(fJson, "widgetSpacing", -1);
        target->widgetBorderRadius = jsonInt(fJson, "widgetBorderRadius", -1);

        std::string fBg = jsonStr(fJson, "backgroundColor", jsonStr(fJson, "background", ""));
        if (!fBg.empty() && parseHexColor(fBg, target->bgR, target->bgG, target->bgB)) {
            target->hasBgColor = true;
            target->backgroundColor = fBg;
        } else if (fJson.contains("backgroundColor") && fJson["backgroundColor"].is_null()) {
            target->hasBgColor = false;
            target->backgroundColor.clear();
        }

        if (fJson.contains("widgets") && fJson["widgets"].is_array()) {
            for (auto& w : target->widgets) w.cleanupTextures();
            target->widgets.clear();
            for (const auto& wJson : fJson["widgets"]) {
                if (wJson.is_object()) {
                    DeckButton parsed = parseMd3Widget(wJson, target->id);
                    auto lit = liveById.find(parsed.widgetId);
                    if (lit != liveById.end()) {
                        copyRuntimeState(lit->second, parsed);
                        if (!parsed.uiTree.is_null()) {
                            extractStateFromUiTree(parsed);
                        }
                    }
                    target->widgets.push_back(std::move(parsed));
                }
            }
        }

        if (target->isDefault || defaultFolderId.empty()) {
            defaultFolderId = target->id;
        }
    }

    // This is the complete snapshot for the requested profile.
    folders_.erase(std::remove_if(folders_.begin(), folders_.end(), [&](DeckFolder& folder) {
        if ((profileId.empty() || folder.profileId == profileId) && !receivedFolders.count(folder.id)) {
            for (auto& widget : folder.widgets) widget.cleanupTextures();
            return true;
        }
        return false;
    }), folders_.end());

    if (profileChanged || !receivedFolders.count(currentFolderId_)) {
        currentFolderId_ = defaultFolderId;
        folderHistory_.clear();
        for (const auto& folder : folders_) {
            if (receivedFolders.count(folder.id) &&
                (folder.id == preferredInitialFolderId_ || folder.name == preferredInitialFolderId_)) {
                currentFolderId_ = folder.id;
                break;
            }
        }
    } else {
        folderHistory_.erase(std::remove_if(folderHistory_.begin(), folderHistory_.end(),
                            [&](const std::string& id) { return !receivedFolders.count(id); }), folderHistory_.end());
    }

    syncActiveFolderToButtons(renderer);
}

void DeckState::handleMd3FolderUpsert(const nlohmann::json& folderJson, SDL_Renderer* renderer) {
    if (!folderJson.is_object()) return;
    std::string fid = jsonStr(folderJson, "id", "");
    if (fid.empty()) return;

    DeckFolder folder;
    folder.id = fid;
    folder.profileId = jsonStr(folderJson, "profileId", currentProfileId_);
    std::string fn = jsonStr(folderJson, "displayName", "");
    if (fn.empty()) fn = jsonStr(folderJson, "name", "");
    if (fn.empty()) fn = jsonStr(folderJson, "title", "");
    if (fn.empty()) fn = jsonStr(folderJson, "folderName", "");
    if (fn.empty()) fn = folder.isDefault ? "Home" : ("Sayfa " + std::to_string(folders_.size() + 1));
    folder.name = fn;
    folder.parentId = jsonStr(folderJson, "parentId", "");
    folder.isDefault = jsonBool(folderJson, "isDefault", false);
    folder.rows = jsonInt(folderJson, "rows", -1);
    folder.cols = jsonInt(folderJson, "columns", -1);
    folder.widgetSpacing = jsonInt(folderJson, "widgetSpacing", -1);
    folder.widgetBorderRadius = jsonInt(folderJson, "widgetBorderRadius", -1);

    std::string fBg = jsonStr(folderJson, "backgroundColor", jsonStr(folderJson, "background", ""));
    if (!fBg.empty() && parseHexColor(fBg, folder.bgR, folder.bgG, folder.bgB)) {
        folder.hasBgColor = true;
        folder.backgroundColor = fBg;
    }

    if (folderJson.contains("widgets") && folderJson["widgets"].is_array()) {
        for (const auto& wJson : folderJson["widgets"]) {
            if (wJson.is_object()) {
                folder.widgets.push_back(parseMd3Widget(wJson, folder.id));
            }
        }
    }

    bool found = false;
    for (auto& existing : folders_) {
        if (existing.id == fid) {
            if (folder.widgets.empty() && !existing.widgets.empty() && !folderJson.contains("widgets")) {
                folder.widgets = std::move(existing.widgets);
            } else {
                for (auto& w : existing.widgets) w.cleanupTextures();
            }
            if (folder.name.empty()) folder.name = existing.name;
            existing = std::move(folder);
            found = true;
            break;
        }
    }
    if (!found) {
        folders_.push_back(std::move(folder));
    }

    if (currentFolderId_ == fid || currentFolderId_.empty()) {
        if (currentFolderId_.empty()) currentFolderId_ = fid;
        syncActiveFolderToButtons(renderer);
    }
}

void DeckState::handleMd3FolderDelete(const std::string& folderId, SDL_Renderer* renderer) {
    for (auto it = folders_.begin(); it != folders_.end(); ++it) {
        if (it->id == folderId) {
            for (auto& w : it->widgets) w.cleanupTextures();
            folders_.erase(it);
            break;
        }
    }
    if (currentFolderId_ == folderId) {
        currentFolderId_.clear();
        for (const auto& folder : folders_) {
            if (!currentProfileId_.empty() && folder.profileId != currentProfileId_) continue;
            if (currentFolderId_.empty() || folder.isDefault) currentFolderId_ = folder.id;
            if (folder.isDefault) break;
        }
        syncActiveFolderToButtons(renderer);
    }
}

bool DeckState::handleMd3WidgetUpsert(const std::string& folderId, const nlohmann::json& widgetJson, SDL_Renderer* renderer) {
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

const DeckButton* DeckState::findSpotifyPlayAction(const DeckButton& player) const {
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

void DeckState::handleMd3WidgetDelete(const std::string& folderId, const std::string& widgetId, SDL_Renderer* renderer) {
    removeCachedResource("b64:" + widgetId);
    std::string targetFolderId = folderId.empty() ? currentFolderId_ : folderId;
    for (auto& f : folders_) {
        if (f.id == targetFolderId) {
            for (auto it = f.widgets.begin(); it != f.widgets.end(); ++it) {
                if (it->widgetId == widgetId) {
                    it->cleanupTextures();
                    f.widgets.erase(it);
                    break;
                }
            }
            break;
        }
    }
    if (targetFolderId == currentFolderId_) {
        syncActiveFolderToButtons(renderer);
    }
}

void DeckState::handleWidgetsDeleted(const std::string& folderId, const std::vector<std::string>& widgetIds, SDL_Renderer* renderer) {
    for (const auto& wid : widgetIds) removeCachedResource("b64:" + wid);
    std::string targetFolderId = folderId.empty() ? currentFolderId_ : folderId;
    std::set<std::string> toDel(widgetIds.begin(), widgetIds.end());
    for (auto& f : folders_) {
        if (f.id == targetFolderId) {
            for (auto it = f.widgets.begin(); it != f.widgets.end();) {
                if (toDel.count(it->widgetId)) {
                    it->cleanupTextures();
                    it = f.widgets.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        }
    }
    if (targetFolderId == currentFolderId_) {
        syncActiveFolderToButtons(renderer);
    }
}

static void pauseInvalidatedMusic(DeckButton& button) {
    if (button.widgetType != "MusicPlayer") return;
    button.progressPositionMs = button.getCurrentProgressMs(SDL_GetTicks64(), button.progressDurationMs > 0);
    button.progressRate = 0;
    button.progressAnchorTicks = 0;
    button.progressAnchorOffsetMs = 0;
    button.musicIsPlaying = false;
}

void DeckState::handleWidgetStateInvalidated(const std::string& widgetId) {
    if (widgetId.empty()) return;
    auto clearSession = [&](DeckButton& b) {
        if (!b.sessionId.empty()) {
            sessionToWidget_.erase(b.sessionId);
            b.sessionId.clear();
        }
        b.revision = 0;
        b.primaryNodeId.clear();
        b.primaryNodeEvents.clear();
        pauseInvalidatedMusic(b);
    };
    if (DeckButton* btn = getButtonById(widgetId)) {
        clearSession(*btn);
    }
    for (auto& f : folders_) {
        for (auto& w : f.widgets) {
            if (w.widgetId == widgetId) clearSession(w);
        }
    }
}

void DeckState::handleUiSessionClosedOrInvalidated(const std::string& sessionId) {
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

bool DeckState::getCurrentFolderBg(uint8_t& r, uint8_t& g, uint8_t& b) const {
    if (currentFolderHasBgColor_) {
        r = currentFolderBgR_;
        g = currentFolderBgG_;
        b = currentFolderBgB_;
        return true;
    }
    return false;
}

void DeckState::syncActiveFolderToButtons(SDL_Renderer* renderer) {
    // Save live state, then rebuild texture bindings from the current cache keys.
    for (auto& b : buttons_) {
        if (!b.widgetId.empty()) {
            for (auto& f : folders_) {
                for (auto& w : f.widgets) {
                    if (w.widgetId == b.widgetId) copyRuntimeState(b, w);
                }
            }
        }
        b.cleanupTextures();
    }
    buttons_.clear();

    currentFolderHasBgColor_ = false;

    auto findFolderById = [&](const std::string& fid) -> const DeckFolder* {
        if (fid.empty()) return nullptr;
        for (const auto& folder : folders_) {
            if (folder.id == fid) return &folder;
        }
        return nullptr;
    };

    for (auto& f : folders_) {
        if (f.id == currentFolderId_) {
            // 2. Resolve folder grid dimensions & styling: 1:1 Macro Deck 3 resolveFolderGrid algorithm
            // chain = [f, parent, grandparent, ...] -> default (Home) folder of profile -> profile defaults -> 3x6
            int resolvedRows = f.rows;
            int resolvedCols = f.cols;
            int resolvedSpacing = f.widgetSpacing;
            int resolvedRadius = f.widgetBorderRadius;

            std::string curParentId = f.parentId;
            for (int depth = 0; depth < 8 && !curParentId.empty(); ++depth) {
                const DeckFolder* parentFolder = findFolderById(curParentId);
                if (!parentFolder) break;
                if (resolvedRows <= 0 && parentFolder->rows > 0) resolvedRows = parentFolder->rows;
                if (resolvedCols <= 0 && parentFolder->cols > 0) resolvedCols = parentFolder->cols;
                if (resolvedSpacing < 0 && parentFolder->widgetSpacing >= 0) resolvedSpacing = parentFolder->widgetSpacing;
                if (resolvedRadius < 0 && parentFolder->widgetBorderRadius >= 0) resolvedRadius = parentFolder->widgetBorderRadius;
                curParentId = parentFolder->parentId;
            }

            // Fallback to the profile's default (Home) folder if still unset
            for (const auto& defF : folders_) {
                if (defF.isDefault && (defF.profileId == currentProfileId_ || defF.profileId.empty())) {
                    if (resolvedRows <= 0 && defF.rows > 0) resolvedRows = defF.rows;
                    if (resolvedCols <= 0 && defF.cols > 0) resolvedCols = defF.cols;
                    if (resolvedSpacing < 0 && defF.widgetSpacing >= 0) resolvedSpacing = defF.widgetSpacing;
                    if (resolvedRadius < 0 && defF.widgetBorderRadius >= 0) resolvedRadius = defF.widgetBorderRadius;
                    break;
                }
            }

            if (resolvedRows <= 0) resolvedRows = (profileDefaultRows_ > 0) ? profileDefaultRows_ : 3;
            if (resolvedCols <= 0) resolvedCols = (profileDefaultCols_ > 0) ? profileDefaultCols_ : 6;
            if (resolvedSpacing < 0) resolvedSpacing = (profileDefaultSpacing_ >= 0) ? profileDefaultSpacing_ : 16;
            if (resolvedRadius < 0) resolvedRadius = (profileDefaultRadius_ >= 0) ? profileDefaultRadius_ : 28;

            if (resolvedRows <= 0) resolvedRows = 3;
            if (resolvedCols <= 0) resolvedCols = 6;
            if (resolvedSpacing < 0) resolvedSpacing = 16;
            if (resolvedRadius < 0) resolvedRadius = 28;

            // Ensure grid dimensions are at least large enough to fit all widgets placed in this folder
            for (const auto& w : f.widgets) {
                resolvedRows = std::max(resolvedRows, w.posY + std::max(1, w.height));
                resolvedCols = std::max(resolvedCols, w.posX + std::max(1, w.width));
            }

            rows_ = std::max(1, resolvedRows);
            cols_ = std::max(1, resolvedCols);
            spacing_ = std::max(0, resolvedSpacing);
            radius_ = std::max(0, resolvedRadius);

            currentFolderHasBgColor_ = f.hasBgColor;
            currentFolderBgR_ = f.bgR;
            currentFolderBgG_ = f.bgG;
            currentFolderBgB_ = f.bgB;

            for (auto& w : f.widgets) {
                if (w.widgetId.empty()) continue;
                DeckButton copy = w;
                if (!copy.uiTree.is_null()) extractStateFromUiTree(copy);
                bindButtonIcon(copy, renderer);
                buttons_.push_back(std::move(copy));
            }
            break;
        }
    }


}

// ============================================================================
// Macro Deck 3 Live Widget UI Tree & Patch Engine
// ============================================================================

void DeckState::registerWidgetSession(const std::string& widgetId, const std::string& sessionId) {
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

struct UiWalkContext {
    bool inHeader = false;
    bool inProvider = false;
    bool inProviderLabels = false;
    bool inTrailing = false;
    bool inBadgeText = false;
    bool inBadgeIcon = false;
    bool inCoverGate = false;
    bool inCoverPlaceholder = false;
    bool inTitleGate = false;
    bool inArtistGate = false;
    bool inAlbumGate = false;
    bool inTimelineGate = false;
    bool inSliderHeader = false;
    bool inSliderValue = false;
    bool inClockTime = false;
    bool inClockDate = false;
    bool inClockZone = false;
    bool inWeatherForecast = false;
    bool inCountdownTime = false;
    bool inCountdownCaption = false;
    bool inCalendarEvent = false;
    bool inCalendarAgenda = false;
    bool inTimerFinished = false;
};

static std::string leafId(const std::string& id) {
    size_t dot = id.find_last_of("./:");
    return (dot == std::string::npos) ? id : id.substr(dot + 1);
}

static bool isHandledLeafType(const std::string& type) {
    return type == "ui.slider" || type == "macrodeck.slider" ||
           type == "ui.gauge" || type == "ui.range-bar" ||
           type == "ui.chart" || type == "ui.icon" ||
           type == "ui.image" || type == "ui.text" ||
           type == "macrodeck.progress-text" || type == "macrodeck.progress-bar";
}

static bool isInternalMusicPlayerSvg(const std::string& rid) {
    if (rid.find("app.macro-deck.music-player.provider.") == 0) return true;
    if (rid == "app.macro-deck.music-player.playing") return true;
    if (rid == "app.macro-deck.music-player.paused") return true;
    if (rid == "app.macro-deck.music-player.disc-spinning") return true;
    if (rid == "app.macro-deck.music-player.warning") return true;
    if (rid == "app.macro-deck.music-player.disc") return true;
    if (rid == "app.macro-deck.music-player.music-note") return true;
    return false;
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

// Convert an ISO 8601 UTC anchor into elapsed milliseconds using the device clock.
static double elapsedFromUtcAnchor(const std::string& anchor) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, used = 0;
    if (std::sscanf(anchor.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n",
                    &year, &month, &day, &hour, &minute, &second, &used) != 6 ||
        used != 19 || year < 2000 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 0;

    double milliseconds = 0;
    if (anchor.size() > 19 && anchor[19] == '.') {
        ++used;
        for (double place = 100; used < static_cast<int>(anchor.size()) &&
             anchor[used] >= '0' && anchor[used] <= '9'; ++used, place *= 0.1) {
            milliseconds += (anchor[used] - '0') * place;
        }
    }
    const auto zone = anchor.substr(used);
    if (zone != "Z" && zone != "+00:00") return 0;
    year -= month <= 2;
    const int era = year / 400;
    const int yearOfEra = year - era * 400;
    const int dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int days = era * 146097 + yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear - 719468;
    const double anchorMs = ((days * 24.0 + hour) * 3600 + minute * 60 + second) * 1000 + milliseconds;
    timespec current{};
    if (clock_gettime(CLOCK_REALTIME, &current) != 0) return 0;
    const double age = current.tv_sec * 1000.0 + current.tv_nsec / 1e6 - anchorMs;
    return std::abs(age) <= 86400000 ? age : 0;
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

static void walkUiTreeContextual(
    const nlohmann::json& node,
    DeckButton& btn,
    UiWalkContext ctx,
    std::vector<std::string>& genericTexts,
    int depth)
{
    if (!node.is_object()) return;

    // Unwrap outer {"root": {...}} wrapper if present
    if (!node.contains("type") && !node.contains("children") && node.contains("root") && node["root"].is_object()) {
        walkUiTreeContextual(node["root"], btn, ctx, genericTexts, depth);
        return;
    }

    std::string id = jsonStr(node, "id", "");
    std::string leaf = leafId(id);
    std::string type = jsonStr(node, "type", "");
    const nlohmann::json props = (node.contains("properties") && node["properties"].is_object())
                                 ? node["properties"] : nlohmann::json::object();

    bool hasChildren = (node.contains("children") && node["children"].is_array() && !node["children"].empty());

    // Update ancestor flags based on Macro Deck 3 Widget View leaf node IDs (composed ids use dot paths)
    if (props.contains("coverStyle") && props["coverStyle"].is_string()) {
        btn.musicCoverStyle = props["coverStyle"].get<std::string>();
    }
    if ((leaf == "smallCoverGate" && hasChildren) || leaf == "smallCover") {
        btn.musicCoverStyle = "small";
    } else if ((leaf == "fullCoverGate" && hasChildren) || leaf == "fullCover" || leaf == "artworkLayer") {
        btn.musicCoverStyle = "full";
    } else if (leaf == "headerGate" || leaf == "header") {
        if (btn.widgetType == "MusicPlayer") ctx.inHeader = true;
        if (btn.widgetType == "Slider") ctx.inSliderHeader = true;
    } else if (leaf == "provider" || leaf == "providerNameGate" || leaf == "providerName") {
        ctx.inProvider = true;
    } else if (leaf == "labelsGate" || leaf == "labels" || leaf == "sourceGate") {
        if (ctx.inProvider || btn.widgetType == "MusicPlayer") ctx.inProviderLabels = true;
    } else if (leaf == "trailingGate" || leaf == "trailing") {
        ctx.inTrailing = true;
    } else if (leaf == "badgeTextGate" || leaf == "badgeText") {
        ctx.inBadgeText = true;
    } else if (leaf == "badgeGate") {
        ctx.inBadgeIcon = true;
    } else if (leaf == "coverGate" || leaf == "artworkGate" || leaf == "artworkLayer" || leaf == "cover") {
        ctx.inCoverGate = true;
        if (btn.widgetType == "MusicPlayer" && hasChildren) {
            if (leaf == "artworkLayer") btn.musicCoverStyle = "full";
            else if (leaf == "coverGate") btn.musicCoverStyle = "small";
        }
    } else if (leaf == "coverPlaceholderGate" || leaf == "coverPlaceholder" || leaf == "placeholderGate" || leaf == "placeholder" || leaf == "placeholderLayer") {
        ctx.inCoverPlaceholder = true;
        if (btn.widgetType == "MusicPlayer" && hasChildren) {
            if (leaf == "placeholderLayer") btn.musicCoverStyle = "full";
            else if (leaf == "coverPlaceholderGate") btn.musicCoverStyle = "small";
        }
    } else if (leaf == "titleGate" || leaf == "title") {
        if (btn.widgetType == "MusicPlayer") ctx.inTitleGate = true;
    } else if (leaf == "artistGate" || leaf == "artist") {
        if (btn.widgetType == "MusicPlayer") ctx.inArtistGate = true;
    } else if (leaf == "albumGate" || leaf == "album") {
        if (btn.widgetType == "MusicPlayer") ctx.inAlbumGate = true;
    } else if (leaf == "timelineGate" || leaf == "timeline" || leaf == "times") {
        ctx.inTimelineGate = true;
    } else if (leaf == "value" || leaf == "lead") {
        if (btn.widgetType == "Slider") ctx.inSliderValue = true;
    }

    // Clock node detection
    if (btn.widgetType == "Clock") {
        if (type == "macrodeck.clock-dial" || (leaf == "analogGate" && hasChildren) || leaf == "analogFace") {
            btn.clockIsAnalog = true;
        } else if ((leaf == "digitalGate" && hasChildren) || leaf == "digitalStack") {
            btn.clockIsAnalog = false;
        } else if (leaf == "time" || leaf == "timeLocalized") {
            ctx.inClockTime = true;
        } else if (leaf == "dateGate" || leaf == "date" || leaf == "dateFallback") {
            ctx.inClockDate = true;
        } else if (leaf == "zoneGate" || leaf == "zone") {
            ctx.inClockZone = true;
        }
    }

    // Countdown & Stopwatch timer node detection
    if (btn.widgetType == "Countdown" || btn.widgetType == "Stopwatch") {
        if (leaf == "finished" || leaf == "finishedTime") {
            btn.countdownIsFinished = true;
            ctx.inTimerFinished = true;
        } else if ((leaf == "clockGate" || leaf == "ringFrame" || leaf == "readout") && hasChildren) {
            btn.countdownIsFinished = false;
        } else if (leaf == "time" || leaf == "timeGate") {
            ctx.inCountdownTime = true;
        } else if (leaf == "captionGate" || leaf == "caption") {
            ctx.inCountdownCaption = true;
        }
    }

    // Weather forecast & style node detection
    if (btn.widgetType == "Weather") {
        if (leaf == "compact" && hasChildren) {
            btn.weatherStyle = "compact";
        } else if ((leaf == "current" || leaf == "forecast") && hasChildren) {
            btn.weatherStyle = "forecast";
        }
        if (leaf == "forecastGate" || leaf == "forecast" || leaf == "days" || leaf == "forecastDays") {
            ctx.inWeatherForecast = true;
        }
        if (ctx.inWeatherForecast && hasChildren &&
            leaf != "forecastGate" && leaf != "forecast" && leaf != "days" && leaf != "forecastDays" && leaf != "label") {
            WeatherForecastDay fday;
            auto inspectForecastNode = [&](auto& self, const nlohmann::json& fn) -> void {
                if (!fn.is_object()) return;
                std::string fid = leafId(jsonStr(fn, "id", ""));
                std::string ftype = jsonStr(fn, "type", "");
                nlohmann::json fprops = (fn.contains("properties") && fn["properties"].is_object()) ? fn["properties"] : nlohmann::json::object();
                if (ftype == "ui.image" || fprops.contains("source")) {
                    std::string frid;
                    if (fprops.contains("source") && fprops["source"].is_object()) {
                        frid = jsonStr(fprops["source"], "resourceId", jsonStr(fprops["source"], "reference", ""));
                    } else if (fprops.contains("source") && fprops["source"].is_string()) {
                        frid = fprops["source"].get<std::string>();
                    } else {
                        frid = jsonStr(fprops, "resourceId", jsonStr(fprops, "reference", ""));
                    }
                    if (!frid.empty()) {
                        fday.resourceId = frid;
                        if (std::find(btn.treeResourceIds.begin(), btn.treeResourceIds.end(), frid) == btn.treeResourceIds.end()) {
                            btn.treeResourceIds.push_back(frid);
                        }
                    }
                }
                std::string ftxt = widgetNodeText(fn, fprops);
                if (!ftxt.empty()) {
                    if (fid == "day" || (fday.label.empty() && fid != "min" && fid != "max")) {
                        fday.label = ftxt;
                    } else if (fid == "min") {
                        fday.minTemp = ftxt;
                    } else if (fid == "max") {
                        fday.maxTemp = ftxt;
                    }
                }
                if (fn.contains("children") && fn["children"].is_array()) {
                    for (const auto& fch : fn["children"]) self(self, fch);
                }
            };
            inspectForecastNode(inspectForecastNode, node);
            if (!fday.label.empty() || !fday.maxTemp.empty() || !fday.minTemp.empty()) {
                btn.weatherForecast.push_back(std::move(fday));
                return;
            }
        }
    }

    // Gauges item detection (cN-grid, g0, g1, g2, rings-grid, bars-rows, gauge-0, ...)
    if (btn.widgetType == "Gauges") {
        if (leaf == "rings" || leaf == "rings-grid" || leaf == "arcGrid" || leaf.find("-grid") != std::string::npos || (leaf == "arcGate" && hasChildren)) {
            btn.gaugeIsArc = true;
        } else if (leaf == "bars" || leaf == "bars-rows" || leaf == "barStack" || leaf == "singleBar" || (leaf == "barGate" && hasChildren)) {
            btn.gaugeIsArc = false;
        }

        auto parseSingleGaugeNode = [&](const nlohmann::json& gRoot) {
            GaugeItem item;
            std::string valPart;
            std::string unitPart;
            std::string readingPart;
            auto inspectGaugeSubtree = [&](auto& self, const nlohmann::json& gnode) -> void {
                if (!gnode.is_object()) return;
                std::string gid = jsonStr(gnode, "id", "");
                std::string gleaf = leafId(gid);
                std::string gtype = jsonStr(gnode, "type", "");
                nlohmann::json gprops = (gnode.contains("properties") && gnode["properties"].is_object()) ? gnode["properties"] : nlohmann::json::object();
                if (gtype == "ui.image" || (gtype == "ui.button" && gprops.contains("source")) || gprops.contains("source")) {
                    std::string grid;
                    if (gprops.contains("source") && gprops["source"].is_object()) {
                        grid = jsonStr(gprops["source"], "resourceId", jsonStr(gprops["source"], "reference", ""));
                    } else if (gprops.contains("source") && gprops["source"].is_string()) {
                        grid = gprops["source"].get<std::string>();
                    } else {
                        grid = jsonStr(gprops, "resourceId", jsonStr(gprops, "reference", ""));
                    }
                    if (!grid.empty()) {
                        item.resourceId = grid;
                        if (std::find(btn.treeResourceIds.begin(), btn.treeResourceIds.end(), grid) == btn.treeResourceIds.end()) {
                            btn.treeResourceIds.push_back(grid);
                        }
                    }
                }
                if (gtype == "ui.gauge" || gleaf == "ring" || gleaf == "dial") {
                    btn.gaugeIsArc = true;
                    double rVal = jsonDouble(gprops, "level", jsonDouble(gprops, "value", item.level));
                    float lvl = static_cast<float>(rVal);
                    if (lvl > 1.0f && lvl <= 100.0f) lvl /= 100.0f;
                    item.level = std::clamp(lvl, 0.0f, 1.0f);
                    std::string col = jsonStr(gprops, "levelColor", jsonStr(gprops, "color", ""));
                    if (!col.empty() && parseHexColor(col, item.r, item.g, item.b)) item.hasColor = true;
                    std::string lbl = jsonStr(gprops, "label", "");
                    std::string val = jsonStr(gprops, "valueText", jsonStr(gprops, "reading", ""));
                    if (!lbl.empty() && item.name.empty()) item.name = lbl;
                    if (!val.empty() && readingPart.empty()) readingPart = val;
                } else if (gtype == "ui.range-bar" || gleaf == "bar" || gleaf.find("-bar") != std::string::npos) {
                    double rVal = jsonDouble(gprops, "end", jsonDouble(gprops, "level", jsonDouble(gprops, "value", item.level)));
                    float lvl = static_cast<float>(rVal);
                    if (lvl > 1.0f && lvl <= 100.0f) lvl /= 100.0f;
                    item.level = std::clamp(lvl, 0.0f, 1.0f);
                    std::string col = jsonStr(gprops, "startColor", jsonStr(gprops, "levelColor", jsonStr(gprops, "color", "")));
                    if (!col.empty() && parseHexColor(col, item.r, item.g, item.b)) item.hasColor = true;
                }
                std::string gtxt = widgetNodeText(gnode, gprops);
                if (!gtxt.empty()) {
                    if (gleaf == "name" || gleaf == "label") {
                        item.name = gtxt;
                    } else if (gleaf == "value") {
                        valPart = gtxt;
                    } else if (gleaf == "unit") {
                        unitPart = gtxt;
                    } else if (gleaf == "reading") {
                        readingPart = gtxt;
                    } else if (gtxt.find('%') != std::string::npos || gtxt.find("GB") != std::string::npos || gtxt.find("MB") != std::string::npos || gtxt.find("°") != std::string::npos || gtxt.find("\xc2\xb0") != std::string::npos || (!gtxt.empty() && isdigit(static_cast<unsigned char>(gtxt[0])))) {
                        if (valPart.empty()) valPart = gtxt;
                        else if (unitPart.empty() && gtxt.size() <= 4) unitPart = gtxt;
                    } else if (item.name.empty()) {
                        item.name = gtxt;
                    }
                }
                if (gnode.contains("children") && gnode["children"].is_array()) {
                    for (const auto& gch : gnode["children"]) self(self, gch);
                }
                if (!isHandledLeafType(gtype) && gnode.contains("fallback") && gnode["fallback"].is_object()) {
                    self(self, gnode["fallback"]);
                }
            };
            inspectGaugeSubtree(inspectGaugeSubtree, gRoot);
            if (!valPart.empty()) {
                item.reading = valPart + unitPart;
            } else if (!readingPart.empty()) {
                item.reading = readingPart;
            }
            if (item.reading.empty() || item.reading == "--") {
                item.reading = std::to_string(static_cast<int>(std::round(item.level * 100.0f))) + "%";
            }
            if (item.name.empty()) {
                size_t gIdx = btn.gauges.size();
                item.name = (gIdx == 0 ? "CPU" : (gIdx == 1 ? "RAM" : (gIdx == 2 ? "GPU" : ("G" + std::to_string(gIdx + 1)))));
            }
            btn.gauges.push_back(std::move(item));
        };

        bool isGridContainer = (leaf == "rings" || leaf == "rings-grid" || leaf == "bars" || leaf == "bars-rows" ||
                                leaf == "arcGrid" || leaf == "barStack" || leaf == "grid" ||
                                leaf.find("-grid") != std::string::npos || leaf.find("-rows") != std::string::npos ||
                                type == "ui.grid");
        if (isGridContainer && hasChildren) {
            bool parsedAny = false;
            for (const auto& gChild : node["children"]) {
                if (gChild.is_object()) {
                    std::string ctype = jsonStr(gChild, "type");
                    std::string cleaf = leafId(jsonStr(gChild, "id"));
                    nlohmann::json chProps = (gChild.contains("properties") && gChild["properties"].is_object()) ? gChild["properties"] : nlohmann::json::object();
                    bool isCell = (ctype == "ui.stack" || ctype == "ui.gauge" || ctype == "ui.dial" || ctype == "ui.range-bar" ||
                                   chProps.contains("level") || chProps.contains("value") ||
                                   (cleaf.size() >= 2 && cleaf[0] == 'g' && isdigit(static_cast<unsigned char>(cleaf[1]))) ||
                                   cleaf.find("gauge") != std::string::npos || cleaf.find("bar") != std::string::npos ||
                                   cleaf.find("ring") != std::string::npos || cleaf == "cpu" || cleaf == "ram" || cleaf == "gpu");
                    if (isCell) {
                        parseSingleGaugeNode(gChild);
                        parsedAny = true;
                    }
                }
            }
            if (parsedAny) return;
        }
        bool isDirectGaugeCell = ((leaf.size() >= 2 && leaf[0] == 'g' && isdigit(static_cast<unsigned char>(leaf[1]))) ||
                                  leaf.rfind("gauge-", 0) == 0 || leaf == "singleBar" || leaf == "dial" ||
                                  leaf == "cpu" || leaf == "ram" || leaf == "gpu" || type == "ui.gauge" || type == "ui.dial");
        if (isDirectGaugeCell) {
            parseSingleGaugeNode(node);
            return;
        }
    }

    // Battery device cell detection (single square/wide/tall/singleBar or multi rows/cols cells)
    if (btn.widgetType == "Battery") {
        auto parseBatteryDeviceNode = [&](const nlohmann::json& bRoot) {
            BatteryDeviceItem dev;
            std::string valPart;
            std::string unitPart;
            auto inspectBatterySubtree = [&](auto& self, const nlohmann::json& bnode) -> void {
                if (!bnode.is_object()) return;
                std::string bid = leafId(jsonStr(bnode, "id", ""));
                std::string btype = jsonStr(bnode, "type", "");
                nlohmann::json bprops = (bnode.contains("properties") && bnode["properties"].is_object()) ? bnode["properties"] : nlohmann::json::object();

                if (btype == "ui.icon") {
                    std::string ic = widgetIconName(bprops);
                    if (bid == "bolt" || ic == "zap" || ic == "battery-charging") {
                        dev.isCharging = true;
                        if (dev.iconName.empty() && ic == "battery-charging") dev.iconName = ic;
                    } else if (!ic.empty() && bid != "glyphFallback") {
                        dev.iconName = ic;
                    }
                }
                if (btype == "ui.icon" && bprops.contains("icon") && bprops["icon"].is_object() &&
                    dev.resourceId.empty()) {
                    dev.resourceId = jsonStr(bprops["icon"], "resourceId");
                }
                if (btype == "ui.image" && bid != "glyphFallback" && dev.resourceId.empty()) {
                    dev.resourceId = widgetArtworkKey(bprops);
                }
                if (btype == "ui.gauge" || bid == "ring") {
                    double rVal = jsonDouble(bprops, "level", jsonDouble(bprops, "value", -1.0));
                    if (rVal >= 0.0) {
                        float lvl = static_cast<float>(rVal);
                        if (lvl > 1.0f && lvl <= 100.0f) lvl /= 100.0f;
                        dev.level = std::clamp(lvl, 0.0f, 1.0f);
                    }
                    std::string col = jsonStr(bprops, "levelColor", jsonStr(bprops, "color", ""));
                    if (!col.empty() && parseHexColor(col, dev.r, dev.g, dev.b)) dev.hasColor = true;
                } else if (btype == "ui.range-bar" || bid == "bar") {
                    double rVal = jsonDouble(bprops, "end", jsonDouble(bprops, "level", jsonDouble(bprops, "value", -1.0)));
                    if (rVal >= 0.0) {
                        float lvl = static_cast<float>(rVal);
                        if (lvl > 1.0f && lvl <= 100.0f) lvl /= 100.0f;
                        dev.level = std::clamp(lvl, 0.0f, 1.0f);
                    }
                    std::string col = jsonStr(bprops, "startColor", jsonStr(bprops, "levelColor", jsonStr(bprops, "color", "")));
                    if (!col.empty() && parseHexColor(col, dev.r, dev.g, dev.b)) dev.hasColor = true;
                }
                std::string btxt = widgetNodeText(bnode, bprops);
                if (!btxt.empty()) {
                    if (bid == "name") dev.name = btxt;
                    else if (bid == "value") valPart = btxt;
                    else if (bid == "unit") unitPart = btxt;
                    else if (bid == "state") dev.stateText = btxt;
                    else if (bid == "hintText" && dev.stateText.empty()) dev.stateText = btxt;
                }
                if (bnode.contains("children") && bnode["children"].is_array()) {
                    for (const auto& bch : bnode["children"]) self(self, bch);
                }
            };
            inspectBatterySubtree(inspectBatterySubtree, bRoot);
            if (!valPart.empty()) dev.valueText = valPart + unitPart;
            else if (dev.level >= 0.0f) dev.valueText = std::to_string(static_cast<int>(std::round(dev.level * 100.0f))) + "%";
            if (!dev.name.empty() || !dev.valueText.empty() || !dev.iconName.empty() || !dev.resourceId.empty() || dev.level >= 0.0f) {
                btn.batteryDevices.push_back(std::move(dev));
            }
        };

        if (leaf == "square" || leaf == "wide" || leaf == "tall" || leaf == "singleBar" ||
            (type == "ui.grid" && hasChildren) || leaf.find("-grid") != std::string::npos || leaf.find("-rows") != std::string::npos) {
            if (leaf == "square" || leaf == "wide" || leaf == "tall" || leaf == "singleBar") {
                parseBatteryDeviceNode(node);
                return;
            }
            if (hasChildren) {
                bool parsedAny = false;
                for (const auto& bChild : node["children"]) {
                    if (bChild.is_object()) {
                        parseBatteryDeviceNode(bChild);
                        parsedAny = true;
                    }
                }
                if (parsedAny) return;
            }
        }
    }

    // Calendar Agenda & NextEvent row detection
    if (btn.widgetType == "Calendar") {
        if (leaf == "event" || leaf == "eventNarrow") ctx.inCalendarEvent = true;
        if (leaf == "glanceRows" || leaf == "compactRows" || leaf == "days" || leaf == "sections") ctx.inCalendarAgenda = true;
        if ((leaf == "glanceRows" || leaf == "compactRows") && hasChildren) {
            for (const auto& row : node["children"]) {
                if (!row.is_object()) continue;
                CalendarEventItem ev;
                auto inspectEvRow = [&](auto& self, const nlohmann::json& rn) -> void {
                    if (!rn.is_object()) return;
                    std::string rl = leafId(jsonStr(rn, "id", ""));
                    nlohmann::json rprops = (rn.contains("properties") && rn["properties"].is_object()) ? rn["properties"] : nlohmann::json::object();
                    std::string rtxt = widgetNodeText(rn, rprops);
                    if (!rtxt.empty()) {
                        if (rl == "title" && ev.title.empty()) ev.title = rtxt;
                        else if (rl == "time" && ev.timeText.empty()) ev.timeText = rtxt;
                        else if (rl == "location" && ev.location.empty()) ev.location = rtxt;
                    }
                    if (rn.contains("children") && rn["children"].is_array()) {
                        for (const auto& ch : rn["children"]) self(self, ch);
                    }
                };
                inspectEvRow(inspectEvRow, row);
                if (!ev.title.empty() || !ev.timeText.empty()) {
                    btn.calendarEvents.push_back(std::move(ev));
                }
            }
        }
    }

    // Handle ui.responsive by picking the active variant for (btn.width, btn.height)
    if (type == "ui.responsive") {
        const nlohmann::json* chosenChild = selectResponsiveChild(node, btn.width, btn.height);
        if (chosenChild) {
            walkUiTreeContextual(*chosenChild, btn, ctx, genericTexts, depth + 1);
        }
        return;
    }

    // Extract events (check both node["events"] and props["events"])
    std::vector<std::string> evs;
    if (node.contains("events") && node["events"].is_array()) {
        for (const auto& e : node["events"]) {
            if (e.is_string()) evs.push_back(e.get<std::string>());
        }
    }
    if (props.contains("events") && props["events"].is_array()) {
        for (const auto& e : props["events"]) {
            if (e.is_string()) evs.push_back(e.get<std::string>());
        }
    }

    if (!id.empty()) {
        bool hasSliderEv = std::find(evs.begin(), evs.end(), "change") != evs.end() ||
                           std::find(evs.begin(), evs.end(), "adjust") != evs.end() ||
                           std::find(evs.begin(), evs.end(), "slide") != evs.end();
        bool isActualSlider = (type == "ui.slider" || type == "macrodeck.slider" ||
                              ((type == "ui.range-bar" || hasSliderEv) && type != "ui.shape" && leaf != "trackFallback"));
        if (isActualSlider && (btn.sliderNodeId.empty() || type == "ui.slider" || hasSliderEv)) {
            btn.sliderNodeId = id;
        }
        if ((btn.primaryNodeId.empty() || ((btn.widgetType == "Countdown" || btn.widgetType == "Stopwatch") && leaf == "timer")) &&
            !hasSliderEv && !isActualSlider) {
            btn.primaryNodeId = id;
            btn.primaryNodeEvents = evs;
        }
    }

    if (type == "ui.button" && !evs.empty() && !id.empty()) {
        // Deduplicate by nodeId
        bool exists = false;
        for (const auto& existingSb : btn.subButtons) {
            if (existingSb.nodeId == id) { exists = true; break; }
        }
        if (!exists) {
            SubButtonNode sb;
            sb.nodeId = id;
            if (node.contains("children") && node["children"].is_array()) {
                for (const auto& ch : node["children"]) {
                    if (ch.is_object() && ch.contains("properties") && ch["properties"].is_object()) {
                        if (jsonStr(ch, "type") == "ui.icon") {
                            sb.iconName = jsonStr(ch["properties"], "icon");
                        } else if (jsonStr(ch, "type") == "ui.text") {
                            sb.label = trimText(jsonStr(ch["properties"], "text"));
                        }
                    }
                }
            }
            btn.subButtons.push_back(sb);
        }
    }

    if (!props.empty()) {
        // Root or primary container / ActionButton background & border
        if (depth <= 1 || leaf == "root" || leaf == "actionButton" || leaf == "musicPlayer" || leaf == "slider" ||
            leaf == "weather" || leaf == "gauges" || leaf == "timer" || leaf == "battery" ||
            leaf == "calendarAgenda" || leaf == "calendarNextEvent" || leaf == "historyGraph" ||
            (btn.widgetType == "ActionButton" && type == "ui.button")) {
            std::string bg = jsonStr(props, "backgroundColor", jsonStr(props, "background", ""));
            if (bg == "transparent" || bg == "none") {
                btn.hasBgColor = false;
            } else if (!bg.empty() && leaf != "finished") {
                uint8_t r, g, b;
                if (parseHexColor(bg, r, g, b)) {
                    btn.bgR = r; btn.bgG = g; btn.bgB = b;
                    btn.hasBgColor = true;
                }
            }
            applyButtonBorder(btn, props);
            if (btn.widgetType == "ActionButton" && (leaf == "actionButton" || type == "ui.button")) {
                std::string just = jsonStr(props, "justify", jsonStr(props, "justifyContent", ""));
                if (just == "start") btn.labelPosition = "top";
                else if (just == "center") btn.labelPosition = "center";
                else if (just == "end") btn.labelPosition = "bottom";
            }
        }

        // Handle built-in vector icon node (ui.icon)
        if (type == "ui.icon") {
            std::string ic = widgetIconName(props);
            if ((btn.widgetType == "Countdown" || btn.widgetType == "Stopwatch") && leaf == "state") {
                if (ic == "play") {
                    btn.countdownIsPaused = true;
                    btn.countdownIsRunning = false;
                    btn.stopwatchIsPaused = true;
                    btn.stopwatchIsRunning = false;
                } else if (ic == "pause") {
                    btn.countdownIsPaused = false;
                    btn.countdownIsRunning = true;
                    btn.stopwatchIsPaused = false;
                    btn.stopwatchIsRunning = true;
                }
            } else if (!ic.empty() && btn.iconName.empty() && leaf != "glyphFallback") {
                btn.iconName = ic;
            }
        }

        // Handle text nodes (including macrodeck.dynamic-text and macrodeck.progress-text with fallback)
        std::string txt = widgetNodeText(node, props);
        if (!txt.empty()) {
            if (btn.widgetType == "MusicPlayer") {
                if (ctx.inProviderLabels || ctx.inProvider) {
                    if (btn.musicProviderName.empty()) btn.musicProviderName = txt;
                    else if (btn.musicSourceName.empty() && txt != btn.musicProviderName) btn.musicSourceName = txt;
                } else if (ctx.inBadgeText || ctx.inTrailing) {
                    btn.musicBadgeText = txt;
                } else if (ctx.inTitleGate) {
                    btn.musicTitle = txt;
                } else if (ctx.inArtistGate) {
                    btn.musicArtist = txt;
                } else if (ctx.inAlbumGate) {
                    btn.musicAlbum = txt;
                } else if (!ctx.inTimelineGate) {
                    genericTexts.push_back(txt);
                }
            } else if (btn.widgetType == "Slider") {
                bool isValText = ctx.inSliderValue || leaf == "value" || (txt.find('%') != std::string::npos) ||
                                 (!txt.empty() && (isdigit(static_cast<unsigned char>(txt[0])) || txt[0] == '.'));
                if (isValText && leaf != "label") {
                    btn.sliderValueText = txt;
                } else if (ctx.inSliderHeader || leaf == "label") {
                    btn.labelText = txt;
                    btn.sliderShowLabel = true;
                } else {
                    genericTexts.push_back(txt);
                }
            } else if (btn.widgetType == "Clock") {
                bool isDynamicTimePlaceholder = (type == "macrodeck.dynamic-text") ||
                                                (props.contains("text") && props["text"].is_object() && props["text"].contains("$time")) ||
                                                (leaf == "timeFallback" || leaf == "dateFallback") ||
                                                (txt == "20:34");
                if (!isDynamicTimePlaceholder) {
                    if (ctx.inClockTime || leaf == "time" || leaf == "timeLocalized") {
                        btn.clockTimeText = txt;
                    } else if (ctx.inClockDate || leaf == "date") {
                        btn.clockDateText = txt;
                    } else if (ctx.inClockZone || leaf == "zone") {
                        btn.clockZoneText = txt;
                    }
                }
            } else if (btn.widgetType == "Weather") {
                auto isDeg = [](const std::string& s) {
                    return (s.find("°") != std::string::npos || s.find("\xc2\xb0") != std::string::npos ||
                            s.find("°C") != std::string::npos || s.find("°F") != std::string::npos);
                };
                bool isPureTemp = !txt.empty() && (isdigit(static_cast<unsigned char>(txt[0])) || txt[0] == '-') && (txt.size() <= 5);
                if (!ctx.inWeatherForecast && (leaf == "high" || leaf == "max" || leaf == "highTemp" ||
                    leaf == "maxTemp" || leaf == "highLabel" || txt.rfind("H:", 0) == 0 || txt.rfind("Y:", 0) == 0)) {
                    btn.weatherHigh = txt;
                } else if (!ctx.inWeatherForecast && (leaf == "low" || leaf == "min" || leaf == "lowTemp" ||
                    leaf == "minTemp" || leaf == "lowLabel" || txt.rfind("L:", 0) == 0 || txt.rfind("D:", 0) == 0)) {
                    btn.weatherLow = txt;
                } else if (leaf == "highLow" || leaf == "minMax" || leaf == "range" || txt.find('/') != std::string::npos) {
                    btn.weatherHighLow = txt;
                } else if (leaf == "temp" || leaf == "temperature" || isDeg(txt) || isPureTemp) {
                    btn.weatherTemp = isDeg(txt) ? txt : (txt + "\xc2\xb0");
                } else if (leaf == "condition" || leaf == "description" || leaf == "summary") {
                    btn.weatherCondition = txt;
                } else if (leaf == "location" || leaf == "city" || leaf == "title") {
                    btn.weatherLocation = txt;
                } else if (leaf == "hint" && btn.weatherCondition.empty()) {
                    btn.weatherCondition = txt;
                } else {
                    genericTexts.push_back(txt);
                }
            } else if (btn.widgetType == "Countdown") {
                if (leaf == "finishedTime") {
                    btn.countdownTimeText = txt;
                    btn.countdownIsFinished = true;
                } else if (leaf == "time" && type == "macrodeck.progress-text") {
                    // Only use static text fallback when not overridden by live $progress
                    if (btn.countdownTimeText.empty()) btn.countdownTimeText = txt;
                } else if ((leaf == "timeFallback" || ctx.inCountdownTime) && leaf != "dismissHint" && leaf != "caption") {
                    if (btn.countdownTimeText.empty()) btn.countdownTimeText = txt;
                } else if (ctx.inCountdownCaption || leaf == "caption") {
                    btn.countdownCaption = txt;
                }
            } else if (btn.widgetType == "Battery") {
                if (leaf == "hintText" && btn.batteryStateText.empty()) {
                    btn.batteryStateText = txt;
                } else if (txt.find('%') != std::string::npos && btn.batteryText.empty()) {
                    btn.batteryText = txt;
                } else {
                    genericTexts.push_back(txt);
                }
            } else if (btn.widgetType == "Stopwatch") {
                if ((leaf == "time" || ctx.inCountdownTime) && (txt.find(':') != std::string::npos || txt.find('.') != std::string::npos)) {
                    if (btn.stopwatchTimeText.empty()) btn.stopwatchTimeText = txt;
                }
            } else if (btn.widgetType == "Calendar") {
                if (leaf == "weekday") btn.calendarWeekdayText = txt;
                else if (leaf == "dayNumber") btn.calendarDayText = txt;
                else if (leaf == "heading") btn.calendarAgendaHeading = txt;
                else if (leaf == "dateLine") btn.calendarNextDate = txt;
                else if (ctx.inCalendarEvent || !ctx.inCalendarAgenda) {
                    if (leaf == "title") btn.calendarNextTitle = txt;
                    else if (leaf == "when") btn.calendarNextWhen = txt;
                    else if (leaf == "range" && btn.calendarNextWhen.empty()) btn.calendarNextWhen = txt;
                    else if (leaf == "location") btn.calendarNextLocation = txt;
                    else genericTexts.push_back(txt);
                }
            } else if (btn.widgetType == "HistoryGraph") {
                if (leaf == "title") {
                    btn.graphTitle = txt;
                    btn.labelText = txt;
                } else if (leaf == "subtitle") {
                    btn.subLabelText = txt;
                } else if (leaf == "value") {
                    btn.sliderValueText = txt;
                } else if (leaf == "unit") {
                    btn.graphUnit = txt;
                } else {
                    genericTexts.push_back(txt);
                }
            } else {
                if (txt.find("şuna değiştir") == std::string::npos && txt.find("Change folder") == std::string::npos) {
                    if (genericTexts.empty() || genericTexts.back() != txt) {
                        genericTexts.push_back(txt);
                    }
                }
                std::string tCol = jsonStr(props, "color", "");
                if (!tCol.empty() && parseHexColor(tCol, btn.textR, btn.textG, btn.textB)) {
                    btn.hasTextColor = true;
                }
                std::string align = jsonStr(props, "textAlign", "");
                if (!align.empty()) btn.textAlign = align;
                int fSize = jsonInt(props, "fontSize", 0);
                if (fSize > 0) btn.fontSize = fSize;
            }
        }

        // Handle image nodes (ui.image) and button artwork (ui.button with source)
        if (type == "ui.image" || props.contains("source")) {
            std::string fit = jsonStr(props, "fit", "");
            if (!fit.empty()) btn.iconFit = fit;

            if (props.contains("zoom") && props["zoom"].is_number()) {
                double z = props["zoom"].get<double>();
                int zoomPct = (z <= 10.0) ? static_cast<int>(std::round(z * 100.0)) : static_cast<int>(std::round(z));
                btn.iconZoom = std::clamp(zoomPct, 25, 250);
            }
            if (props.contains("offsetX") && props["offsetX"].is_number()) {
                double ox = props["offsetX"].get<double>();
                int oxPct = (std::fabs(ox) <= 1.0) ? static_cast<int>(std::round(ox * 100.0)) : static_cast<int>(std::round(ox));
                btn.iconOffsetX = std::clamp(oxPct, -50, 50);
            }
            if (props.contains("offsetY") && props["offsetY"].is_number()) {
                double oy = props["offsetY"].get<double>();
                int oyPct = (std::fabs(oy) <= 1.0) ? static_cast<int>(std::round(oy * 100.0)) : static_cast<int>(std::round(oy));
                btn.iconOffsetY = std::clamp(oyPct, -50, 50);
            }
            if (props.contains("opacity") && props["opacity"].is_number()) {
                double op = props["opacity"].get<double>();
                int opPct = (op <= 1.0) ? static_cast<int>(std::round(op * 100.0)) : static_cast<int>(std::round(op));
                btn.iconOpacity = std::clamp(opPct, 5, 100);
            }

            const std::string sourceId = widgetArtworkId(props);
            std::string rid = widgetArtworkKey(props);

            if (!rid.empty()) {
                if (std::find(btn.treeResourceIds.begin(), btn.treeResourceIds.end(), rid) == btn.treeResourceIds.end()) {
                    btn.treeResourceIds.push_back(rid);
                }
                if (btn.widgetType == "MusicPlayer") {
                    if (ctx.inBadgeIcon || ctx.inTrailing) {
                        if (rid.find("playing") != std::string::npos || rid.find("disc-spinning") != std::string::npos) {
                            btn.musicIsPlaying = true;
                        } else if (rid.find("paused") != std::string::npos) {
                            btn.musicIsPaused = true;
                        } else if (rid.find("warning") != std::string::npos) {
                            btn.musicHasWarning = true;
                        }
                    }
                    if (!isInternalMusicPlayerSvg(sourceId) &&
                        (ctx.inCoverGate || sourceId.find("artwork") != std::string::npos || leaf == "cover")) {
                        btn.resourceId = rid;
                        btn.musicArtworkId = sourceId;
                    } else if (btn.resourceId.empty() && !ctx.inProvider && !ctx.inBadgeIcon && !ctx.inTrailing &&
                               !isInternalMusicPlayerSvg(sourceId)) {
                        btn.resourceId = rid;
                        btn.musicArtworkId = sourceId;
                    }
                } else {
                    if (btn.widgetType == "Weather") {
                        if (!ctx.inWeatherForecast && (btn.resourceId.empty() || leaf == "icon" || leaf == "weatherIcon")) btn.resourceId = rid;
                    } else if (btn.resourceId.empty() || !isInternalMusicPlayerSvg(sourceId)) {
                        btn.resourceId = rid;
                    }
                }
            }
        }

        // Handle Countdown / Stopwatch ring / gauge node
        if ((btn.widgetType == "Countdown" || btn.widgetType == "Stopwatch") &&
            (type == "ui.gauge" || leaf == "ring") && leaf != "ringFallback") {
            double lvl = jsonDouble(props, "level", jsonDouble(props, "value", -1.0));
            if (lvl >= 0.0) {
                btn.countdownRingLevel = std::clamp(static_cast<float>(lvl), 0.0f, 1.0f);
            }
            std::string col = jsonStr(props, "levelColor", jsonStr(props, "color", ""));
            uint8_t cr = 0, cg = 0, cb = 0;
            if (!col.empty() && parseHexColor(col, cr, cg, cb)) {
                if (cr == 138 && cg == 138 && cb == 138) {
                    // #8a8a8a = TimerWidgetView paused tint; keep accentR/G/B clean
                    btn.countdownIsPaused = true;
                    btn.stopwatchIsPaused = true;
                } else {
                    btn.countdownIsPaused = false;
                    btn.stopwatchIsPaused = false;
                    btn.accentR = cr;
                    btn.accentG = cg;
                    btn.accentB = cb;
                    btn.hasAccentColor = true;
                }
            }
        }

        // Handle ui.chart (Macro Deck HistoryGraph)
        if ((type == "ui.chart" || leaf == "chart") && leaf != "chartFallback") {
            if (props.contains("points") && props["points"].is_array()) {
                btn.graphHistory.clear();
                for (const auto& pt : props["points"]) {
                    if (pt.is_number()) {
                        btn.graphHistory.push_back(std::clamp(pt.get<float>(), 0.0f, 1.0f));
                    }
                }
            }
            double lvl = jsonDouble(props, "level", jsonDouble(props, "value", -1.0));
            if (lvl >= 0.0) {
                float fLvl = static_cast<float>(lvl);
                if (fLvl > 1.0f && fLvl <= 100.0f) fLvl /= 100.0f;
                btn.sliderLevel = std::clamp(fLvl, 0.0f, 1.0f);
            }
            std::string col = jsonStr(props, "color", jsonStr(props, "strokeColor", ""));
            if (!col.empty() && parseHexColor(col, btn.accentR, btn.accentG, btn.accentB)) {
                btn.hasAccentColor = true;
            }
        }

        // Handle macrodeck.progress-bar and macrodeck.progress-text ($progress live interpolation)
        if (type == "macrodeck.progress-bar" || type == "macrodeck.progress-text" || type == "ui.range-bar" || leaf == "progress") {
            btn.hasProgressBar = true;
            std::string barColor = jsonStr(props, "startColor", jsonStr(props, "endColor", jsonStr(props, "color", "")));
            if (!barColor.empty()) {
                if (parseHexColor(barColor, btn.accentR, btn.accentG, btn.accentB)) {
                    btn.hasAccentColor = true;
                }
            }

            const nlohmann::json* progObj = nullptr;
            if (props.contains("value") && props["value"].is_object() && props["value"].contains("$progress")) {
                progObj = &props["value"]["$progress"];
            } else if (props.contains("$progress") && props["$progress"].is_object()) {
                progObj = &props["$progress"];
            }

            if (progObj && progObj->is_object()) {
                btn.treeHasProgress = true;
                double newPos = jsonDouble(*progObj, "positionMs", 0.0);
                double newDur = jsonDouble(*progObj, "durationMs", 0.0);
                double newRate = jsonDouble(*progObj, "rate", 1.0);
                std::string newAnchor = jsonStr(*progObj, "anchor", "");

                bool changed = (newAnchor != btn.progressAnchorStr) ||
                               (std::fabs(newPos - (btn.progressPositionMs - btn.progressAnchorOffsetMs)) > 50.0) ||
                               (std::fabs(newRate - btn.progressRate) > 1e-4) ||
                               (std::fabs(newDur - btn.progressDurationMs) > 50.0);

                if (changed || btn.progressAnchorTicks == 0) {
                    btn.progressAnchorOffsetMs = btn.widgetType == "MusicPlayer"
                        ? elapsedFromUtcAnchor(newAnchor) * newRate : 0.0;
                }
                btn.progressPositionMs = newPos + btn.progressAnchorOffsetMs;
                btn.progressDurationMs = newDur;
                btn.progressRate = newRate;
                if (changed || btn.progressAnchorTicks == 0) {
                    btn.progressAnchorStr = newAnchor;
                    btn.progressAnchorTicks = SDL_GetTicks64();
                }

                if (btn.widgetType == "Countdown") {
                    if (newRate > 0.0) {
                        btn.countdownIsRunning = true;
                        btn.countdownIsPaused = false;
                        btn.countdownIsFinished = false;
                    } else if (newPos > 0.0 && newDur > 0.0 && newPos < newDur) {
                        btn.countdownIsRunning = false;
                        btn.countdownIsPaused = true;
                    } else if (newPos <= 0.0) {
                        btn.countdownIsRunning = false;
                        btn.countdownIsPaused = false;
                    }
                } else if (btn.widgetType == "Stopwatch") {
                    btn.stopwatchIsRunning = (newRate > 0.0);
                    btn.stopwatchIsPaused = (newRate == 0.0 && newPos > 0.0);
                }
            } else if (props.contains("value") && props["value"].is_number()) {
                if (SDL_GetTicks() - btn.lastLocalSlideTick >= 350) {
                    float v = props["value"].get<float>();
                    if (v > 1.0f && v <= 100.0f) v /= 100.0f;
                    btn.sliderLevel = std::clamp(v, 0.0f, 1.0f);
                }
            }
        }

        // Handle Slider node (ui.slider)
        bool isSliderNode = (type == "ui.slider" || type == "macrodeck.slider" ||
                            ((type == "ui.range-bar" || type.find("slider") != std::string::npos) && type != "ui.shape" && leaf != "trackFallback"));
        if (isSliderNode && leaf != "trackFallback") {
            if (!id.empty()) {
                if (btn.sliderNodeId.empty() || type == "ui.slider" || leaf == "track") {
                    btn.sliderNodeId = id;
                }
                if (btn.primaryNodeId.empty()) btn.primaryNodeId = id;
            }
            double val = -999999.0;
            auto tryGetVal = [&](const char* propName) {
                if (val != -999999.0 || !props.contains(propName)) return;
                if (props[propName].is_number()) {
                    val = props[propName].get<double>();
                } else if (props[propName].is_string()) {
                    try { val = std::stod(props[propName].get<std::string>()); } catch (...) {}
                }
            };
            tryGetVal("level");
            tryGetVal("value");
            tryGetVal("current");
            tryGetVal("percent");
            tryGetVal("position");

            if (val != -999999.0) {
                float lvl = 0.0f;
                if (btn.sliderMax > btn.sliderMin && val > 1.0) {
                    lvl = static_cast<float>((val - btn.sliderMin) / (btn.sliderMax - btn.sliderMin));
                } else if (val > 1.0 && val <= 100.0) {
                    lvl = static_cast<float>(val / 100.0);
                } else {
                    lvl = static_cast<float>(val);
                }
                if (SDL_GetTicks() - btn.lastLocalSlideTick >= 350) {
                    btn.sliderLevel = std::clamp(lvl, 0.0f, 1.0f);
                }
                btn.hasProgressBar = true;
            }
            std::string lvlCol = jsonStr(props, "levelColor", jsonStr(props, "color", ""));
            if (!lvlCol.empty() && parseHexColor(lvlCol, btn.accentR, btn.accentG, btn.accentB)) {
                btn.hasAccentColor = true;
            }
            std::string dir = jsonStr(props, "direction", "");
            if (!dir.empty()) {
                btn.sliderIsVertical = (dir == "vertical" || dir == "Vertical");
            }
            std::string inter = jsonStr(props, "interaction", "");
            if (!inter.empty()) {
                btn.sliderIsRelative = (inter == "relative" || inter == "Relative");
            }
            if (props.contains("step") && props["step"].is_number()) {
                btn.sliderStep = props["step"].get<double>();
            }
        }
    }

    // For ui.first-fit, children are alternative density candidates of the SAME content;
    // walk only children[0] (the primary full-fidelity layout) so texts and subButtons are never duplicated!
    if (type == "ui.first-fit") {
        if (hasChildren) {
            walkUiTreeContextual(node["children"][0], btn, ctx, genericTexts, depth + 1);
        }
        return;
    }

    if (hasChildren) {
        for (const auto& child : node["children"]) {
            walkUiTreeContextual(child, btn, ctx, genericTexts, depth + 1);
        }
    }

    // Only walk fallback if this node isn't an already-handled native component
    if (!hasChildren && !isHandledLeafType(type) && node.contains("fallback") && node["fallback"].is_object()) {
        walkUiTreeContextual(node["fallback"], btn, ctx, genericTexts, depth + 1);
    }
}

static void addTreeArtwork(const nlohmann::json& node, std::set<std::string>& resources, int depth = 0) {
    if (!node.is_object() || depth > 48 || resources.size() >= 96) return;
    const auto props = node.find("properties");
    if (props != node.end() && props->is_object()) {
        const auto face = jsonStr(*props, "fontFace");
        if (!face.empty() && face.size() <= 160) resources.insert("font:" + face);
    }
    if (props != node.end() && props->is_object()) {
        auto id = widgetArtworkKey(*props);
        if (!id.empty()) resources.insert(std::move(id));
    }
    if (node.contains("children") && node["children"].is_array()) {
        for (const auto& child : node["children"]) addTreeArtwork(child, resources, depth + 1);
    }
    if (node.contains("fallback")) addTreeArtwork(node["fallback"], resources, depth + 1);
}

void DeckState::extractStateFromUiTree(DeckButton& btn) {
    if (btn.uiTree.is_null() || !btn.uiTree.is_object()) return;

    btn.primaryNodeId.clear();
    btn.primaryNodeEvents.clear();
    btn.sliderNodeId.clear();
    btn.subButtons.clear();
    btn.treeResourceIds.clear();
    btn.treeHasProgress = false;
    btn.hasAccentColor = false;

    std::vector<GaugeItem> prevGauges;
    std::vector<WeatherForecastDay> prevWeatherForecast;
    if (btn.widgetType == "MusicPlayer") {
        btn.musicProviderName.clear();
        btn.musicSourceName.clear();
        btn.musicBadgeText.clear();
        btn.musicTitle.clear();
        btn.musicArtist.clear();
        btn.musicAlbum.clear();
        btn.musicIsPlaying = false;
        btn.musicIsPaused = false;
        btn.musicHasWarning = false;
        btn.musicArtworkId.clear();
        btn.musicArtworkKey.clear();
        btn.resourceId.clear();
    } else if (btn.widgetType == "Slider") {
        btn.sliderValueText.clear();
        btn.labelText = btn.staticLabel;
    } else if (btn.widgetType == "Clock") {
        btn.clockIsAnalog = btn.clockConfiguredAnalog;
        btn.clockTimeText.clear();
        btn.clockDateText.clear();
    } else if (btn.widgetType == "Weather") {
        btn.weatherHigh.clear();
        btn.weatherLow.clear();
        prevWeatherForecast = btn.weatherForecast;
        btn.weatherTemp.clear();
        btn.weatherCondition.clear();
        btn.weatherHighLow.clear();
        btn.weatherForecast.clear();
    } else if (btn.widgetType == "Gauges") {
        prevGauges = btn.gauges;
        btn.gauges.clear();
    } else if (btn.widgetType == "Countdown") {
        btn.countdownTimeText.clear();
        btn.countdownIsFinished = false;
        btn.countdownIsPaused = false;
        btn.countdownIsRunning = false;
        btn.hasAccentColor = false;
        btn.accentR = 33; btn.accentG = 150; btn.accentB = 243;
    } else if (btn.widgetType == "Stopwatch") {
        btn.stopwatchTimeText.clear();
        btn.stopwatchIsRunning = false;
        btn.stopwatchIsPaused = false;
    } else if (btn.widgetType == "Battery") {
        btn.batteryDevices.clear();
        btn.batteryText.clear();
        btn.batteryStateText.clear();
        btn.batteryLevel = -1.0f;
        btn.batteryIsCharging = false;
        btn.iconName.clear();
    } else if (btn.widgetType == "Calendar") {
        btn.calendarWeekdayText.clear();
        btn.calendarDayText.clear();
        btn.calendarAgendaHeading.clear();
        btn.calendarEvents.clear();
        btn.calendarNextDate.clear();
        btn.calendarNextTitle.clear();
        btn.calendarNextWhen.clear();
        btn.calendarNextLocation.clear();
    }

    std::vector<std::string> genericTexts;
    UiWalkContext ctx;
    walkUiTreeContextual(btn.uiTree, btn, ctx, genericTexts, 0);
    if (btn.widgetType == "Weather") fillWeatherDetails(btn.uiTree, btn, false);
    std::set<std::string> artwork(btn.treeResourceIds.begin(), btn.treeResourceIds.end());
    addTreeArtwork(btn.uiTree, artwork);
    btn.treeResourceIds.assign(artwork.begin(), artwork.end());

    if (btn.widgetType == "Battery") {
        for (const auto& device : btn.batteryDevices) {
            if (!device.resourceId.empty()) artwork.insert(device.resourceId);
        }
        btn.treeResourceIds.assign(artwork.begin(), artwork.end());
    }

    if (btn.widgetType == "Gauges") {
        if (btn.gauges.empty() && !prevGauges.empty()) {
            btn.gauges = prevGauges;
        } else {
            for (size_t i = 0; i < btn.gauges.size() && i < prevGauges.size(); ++i) {
                if (btn.gauges[i].name.empty() && !prevGauges[i].name.empty()) {
                    btn.gauges[i].name = prevGauges[i].name;
                }
                if (btn.gauges[i].resourceId.empty() && !prevGauges[i].resourceId.empty()) {
                    btn.gauges[i].resourceId = prevGauges[i].resourceId;
                }
                if ((btn.gauges[i].reading.empty() || btn.gauges[i].reading == "--") && !prevGauges[i].reading.empty() && prevGauges[i].reading != "--") {
                    btn.gauges[i].reading = prevGauges[i].reading;
                }
                if (!btn.gauges[i].hasColor && prevGauges[i].hasColor) {
                    btn.gauges[i].r = prevGauges[i].r;
                    btn.gauges[i].g = prevGauges[i].g;
                    btn.gauges[i].b = prevGauges[i].b;
                    btn.gauges[i].hasColor = true;
                }
            }
        }
        static const SDL_Color defaultGaugeColors[4] = {
            {59, 130, 246, 255},  // Blue (CPU)
            {168, 85, 247, 255},  // Purple (RAM)
            {34, 197, 94, 255},   // Green (GPU)
            {245, 158, 11, 255}   // Amber
        };
        for (size_t i = 0; i < btn.gauges.size(); ++i) {
            auto& gi = btn.gauges[i];
            if (!gi.hasColor) {
                const auto& dc = defaultGaugeColors[i % 4];
                gi.r = dc.r; gi.g = dc.g; gi.b = dc.b;
                gi.hasColor = true;
            }
            if (gi.reading.empty() || gi.reading == "--") {
                gi.reading = std::to_string(static_cast<int>(std::round(gi.level * 100.0f))) + "%";
            }
            if (!gi.resourceId.empty()) {
                if (std::find(btn.treeResourceIds.begin(), btn.treeResourceIds.end(), gi.resourceId) == btn.treeResourceIds.end()) {
                    btn.treeResourceIds.push_back(gi.resourceId);
                }
            }
        }
    }

    if (btn.widgetType == "MusicPlayer") {
        if (btn.musicProviderName.empty()) btn.musicProviderName = "Spotify";
        if (btn.musicTitle.empty() && !genericTexts.empty()) {
            if (genericTexts.size() >= 2 && (genericTexts[0] == "Spotify" || genericTexts[0] == "Music")) {
                btn.musicTitle = genericTexts[1];
                if (genericTexts.size() > 2) btn.musicArtist = genericTexts[2];
                if (genericTexts.size() > 3) btn.musicAlbum = genericTexts[3];
            } else {
                btn.musicTitle = genericTexts[0];
                if (genericTexts.size() > 1) btn.musicArtist = genericTexts[1];
                if (genericTexts.size() > 2) btn.musicAlbum = genericTexts[2];
            }
        }
        const auto nothingPlaying = localizedWidgetText({{"scope", "macrodeck.app"}, {"key", "Widgets.MusicPlayer.NothingPlaying"}});
        btn.musicHasTrack = !btn.musicTitle.empty() &&
                            btn.musicTitle != nothingPlaying &&
                            btn.musicTitle != "Nothing playing" &&
                            btn.musicTitle != "No media session";
        if (!btn.treeHasProgress || !btn.musicHasTrack) {
            btn.progressPositionMs = 0;
            btn.progressDurationMs = 0;
            btn.progressRate = 0;
            btn.progressAnchorTicks = 0;
            btn.progressAnchorOffsetMs = 0;
            btn.progressAnchorStr.clear();
        } else if (btn.musicIsPaused || btn.musicHasWarning) {
            btn.progressPositionMs = std::max(0.0, btn.progressPositionMs - btn.progressAnchorOffsetMs);
            btn.progressAnchorOffsetMs = 0;
            btn.progressRate = 0;
        }
        btn.musicIsPlaying = btn.musicHasTrack && btn.progressRate > 0;
        if (btn.musicHasTrack && !btn.musicArtworkId.empty() && btn.resourceId == btn.musicArtworkId) {
            const auto identity = btn.musicTitle + "\n" + btn.musicArtist + "\n" + btn.musicAlbum;
            btn.musicArtworkKey = btn.musicArtworkId + "\ntrack:" + std::to_string(std::hash<std::string>{}(identity));
            artwork.erase(btn.resourceId);
            btn.resourceId = btn.musicArtworkKey;
            artwork.insert(btn.resourceId);
            btn.treeResourceIds.assign(artwork.begin(), artwork.end());
        }
        btn.labelText = btn.musicTitle;
        btn.subLabelText = btn.musicArtist;
        btn.thirdLabelText = btn.musicAlbum;
    } else if (btn.widgetType == "Slider") {
        if (btn.sliderValueText.empty()) {
            for (auto it = genericTexts.begin(); it != genericTexts.end(); ) {
                if (it->find('%') != std::string::npos || (!it->empty() && isdigit(static_cast<unsigned char>((*it)[0])))) {
                    btn.sliderValueText = *it;
                    it = genericTexts.erase(it);
                    break;
                } else {
                    ++it;
                }
            }
        }
        if (btn.labelText.empty() && !btn.staticLabel.empty()) {
            btn.labelText = btn.staticLabel;
        }
        if (btn.labelText.empty() && !genericTexts.empty()) {
            btn.labelText = genericTexts[0];
        }
        // Fallback: If sliderValueText was parsed (e.g. "26 %" or "26%"), extract level from text!
        if (!btn.sliderValueText.empty() && (SDL_GetTicks() - btn.lastLocalSlideTick >= 350)) {
            std::string numStr;
            for (char ch : btn.sliderValueText) {
                if ((ch >= '0' && ch <= '9') || ch == '.') numStr += ch;
                else if (!numStr.empty()) break;
            }
            if (!numStr.empty()) {
                try {
                    double numVal = std::stod(numStr);
                    float lvl = 0.0f;
                    if (btn.sliderMax > btn.sliderMin && numVal > 1.0) {
                        lvl = static_cast<float>((numVal - btn.sliderMin) / (btn.sliderMax - btn.sliderMin));
                    } else if (numVal > 1.0 && numVal <= 100.0) {
                        lvl = static_cast<float>(numVal / 100.0);
                    } else {
                        lvl = static_cast<float>(numVal);
                    }
                    btn.sliderLevel = std::clamp(lvl, 0.0f, 1.0f);
                } catch (...) {}
            }
        }
    } else if (btn.widgetType == "Clock") {
        if (btn.clockTimeText.empty() && !genericTexts.empty()) {
            btn.clockTimeText = genericTexts[0];
            if (btn.clockDateText.empty() && genericTexts.size() > 1) btn.clockDateText = genericTexts[1];
            if (btn.clockZoneText.empty() && genericTexts.size() > 2) btn.clockZoneText = genericTexts[2];
        }
        btn.labelText = btn.clockTimeText;
        btn.subLabelText = btn.clockDateText;
        btn.thirdLabelText = btn.clockZoneText;
    } else if (btn.widgetType == "Weather") {
        for (const auto& gt : genericTexts) {
            bool hasDeg = (gt.find("°") != std::string::npos || gt.find("\xc2\xb0") != std::string::npos ||
                           gt.find("°C") != std::string::npos || gt.find("°F") != std::string::npos);
            bool isDigitStart = !gt.empty() && (isdigit(static_cast<unsigned char>(gt[0])) || gt[0] == '-');
            bool isHighLow = (gt.find('/') != std::string::npos);

            if (isHighLow) {
                btn.weatherHighLow = gt;
            } else if (btn.weatherTemp.empty() && (hasDeg || (isDigitStart && gt.size() <= 6))) {
                btn.weatherTemp = hasDeg ? gt : (gt + "\xc2\xb0");
            } else if (btn.weatherLocation.empty() && (gt == "Istanbul" || gt == "İstanbul" || gt.size() >= 10)) {
                btn.weatherLocation = gt;
            } else if (btn.weatherCondition.empty()) {
                btn.weatherCondition = gt;
            } else if (btn.weatherLocation.empty()) {
                btn.weatherLocation = gt;
            } else if (btn.weatherHighLow.empty()) {
                btn.weatherHighLow = gt;
            }
        }
        if (btn.weatherTemp.empty() && !btn.staticLabel.empty() &&
            btn.staticLabel.find_first_of("0123456789") == 0) {
            std::string sl = btn.staticLabel;
            btn.weatherTemp = (sl.find("°") != std::string::npos || sl.find("\xc2\xb0") != std::string::npos) ? sl : (sl + "\xc2\xb0");
        }
        if (btn.weatherForecast.empty() && !prevWeatherForecast.empty()) {
            btn.weatherForecast = prevWeatherForecast;
        }
        if (!btn.weatherForecast.empty()) {
            if (btn.weatherHigh.empty()) btn.weatherHigh = btn.weatherForecast.front().maxTemp;
            if (btn.weatherLow.empty()) btn.weatherLow = btn.weatherForecast.front().minTemp;
        }
        if (btn.weatherHighLow.empty() && (!btn.weatherHigh.empty() || !btn.weatherLow.empty())) {
            const auto pointed = [](const std::string& value, const char* prefix) {
                return value.empty() || value.find(':') != std::string::npos ? value : prefix + value;
            };
            const auto high = pointed(btn.weatherHigh, "H: ");
            const auto low = pointed(btn.weatherLow, "L: ");
            btn.weatherHighLow = high + (!high.empty() && !low.empty() ? " / " : "") + low;
        }
        btn.labelText = btn.weatherTemp;
        btn.subLabelText = btn.weatherCondition;
        btn.thirdLabelText = btn.weatherLocation;
    } else if (btn.widgetType == "Countdown") {
        if (btn.countdownTimeText.empty() && !genericTexts.empty()) {
            btn.countdownTimeText = genericTexts[0];
        }
        btn.labelText = btn.countdownTimeText;
        btn.subLabelText = btn.countdownCaption;
    } else if (btn.widgetType == "Battery") {
        if (!btn.batteryDevices.empty()) {
            const auto& d0 = btn.batteryDevices[0];
            btn.batteryLevel = d0.level;
            btn.batteryIsCharging = d0.isCharging;
            if (!d0.valueText.empty()) btn.batteryText = d0.valueText;
            if (!d0.stateText.empty()) btn.batteryStateText = d0.stateText;
            if (btn.iconName.empty() && !d0.iconName.empty()) btn.iconName = d0.iconName;
        } else if (!genericTexts.empty() && btn.batteryText.empty()) {
            for (const auto& gt : genericTexts) {
                if (gt.find('%') != std::string::npos) {
                    btn.batteryText = gt;
                    break;
                }
            }
        }
    } else if (btn.widgetType == "Stopwatch") {
        if (btn.stopwatchTimeText.empty() && !genericTexts.empty()) {
            btn.stopwatchTimeText = genericTexts[0];
        }
    } else if (btn.widgetType == "Calendar") {
        if (btn.calendarWeekdayText.empty() && genericTexts.size() >= 2) {
            btn.calendarMonthText = genericTexts[0];
            btn.calendarDayText = genericTexts[1];
            if (genericTexts.size() >= 3) btn.calendarWeekdayText = genericTexts[2];
        }
    } else if (btn.widgetType == "HistoryGraph") {
        for (const auto& gt : genericTexts) {
            if (gt.find('%') != std::string::npos || (!gt.empty() && isdigit(static_cast<unsigned char>(gt[0])) && gt.size() <= 7)) {
                btn.sliderValueText = gt;
            } else if (btn.labelText.empty()) {
                btn.labelText = gt;
            } else if (btn.subLabelText.empty()) {
                btn.subLabelText = gt;
            }
        }
        if (btn.labelText.empty() && !btn.staticLabel.empty()) {
            btn.labelText = btn.staticLabel;
        }
        if (btn.sliderValueText.empty()) {
            if (!btn.graphHistory.empty()) {
                int pct = static_cast<int>(std::round(btn.graphHistory.back() * 100.0f));
                btn.sliderValueText = std::to_string(pct) + " %";
            } else if (btn.sliderLevel > 0.0f) {
                int pct = static_cast<int>(std::round(btn.sliderLevel * 100.0f));
                btn.sliderValueText = std::to_string(pct) + " %";
            }
        }
    } else {
        if (!genericTexts.empty()) {
            btn.labelText = genericTexts[0];
            btn.subLabelText = (genericTexts.size() > 1) ? genericTexts[1] : btn.staticSubLabel;
            btn.thirdLabelText = (genericTexts.size() > 2) ? genericTexts[2] : "";
        } else {
            btn.labelText = btn.staticLabel;
            btn.subLabelText = btn.staticSubLabel;
            btn.thirdLabelText.clear();
        }
    }

    if (!btn.resourceId.empty() || !btn.loadedResourceId.empty()) bindResourceIcon(btn);
}

void DeckState::handleUiSessionTreeUpdated(const std::string& sessionId, int revision, const nlohmann::json& tree) {
    std::string wid;
    auto it = sessionToWidget_.find(sessionId);
    if (it != sessionToWidget_.end()) {
        wid = it->second;
    } else {
        for (const auto& b : buttons_) {
            if (b.sessionId == sessionId) {
                wid = b.widgetId;
                sessionToWidget_[sessionId] = wid;
                break;
            }
        }
    }
    if (wid.empty()) return;

    nlohmann::json rootTree = (tree.is_object() && tree.contains("root") && tree["root"].is_object())
                              ? tree["root"] : tree;

    if (DeckButton* btn = getButtonById(wid)) {
        if (btn->sessionId == sessionId && revision < btn->revision) return;
        btn->sessionId = sessionId;
        btn->revision = revision;
        btn->uiTree = rootTree;
        extractStateFromUiTree(*btn);
    }

    for (auto& f : folders_) {
        for (auto& w : f.widgets) {
            if (w.widgetId == wid) {
                w.sessionId = sessionId;
                w.revision = revision;
                w.uiTree = rootTree;
                extractStateFromUiTree(w);
            }
        }
    }
}

static bool patchSetProperties(nlohmann::json& node, const std::string& targetId, const nlohmann::json& props, const nlohmann::json& removedProps) {
    if (!node.is_object()) return false;
    if (jsonStr(node, "id", "") == targetId) {
        if (!node.contains("properties") || !node["properties"].is_object()) {
            node["properties"] = nlohmann::json::object();
        }
        if (props.is_object()) {
            for (auto it = props.begin(); it != props.end(); ++it) {
                node["properties"][it.key()] = it.value();
            }
        }
        if (removedProps.is_array()) {
            for (const auto& rm : removedProps) {
                if (rm.is_string()) {
                    node["properties"].erase(rm.get<std::string>());
                }
            }
        }
        return true;
    }
    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"]) {
            if (patchSetProperties(child, targetId, props, removedProps)) return true;
        }
    }
    if (node.contains("fallback") && node["fallback"].is_object()) {
        if (patchSetProperties(node["fallback"], targetId, props, removedProps)) return true;
    }
    return false;
}

static bool patchInsertNode(nlohmann::json& node, const std::string& parentId, int index, const nlohmann::json& newNode) {
    if (!node.is_object()) return false;
    if (jsonStr(node, "id", "") == parentId) {
        if (!node.contains("children") || !node["children"].is_array()) {
            node["children"] = nlohmann::json::array();
        }
        auto& children = node["children"];
        if (index < 0 || index >= static_cast<int>(children.size())) {
            children.push_back(newNode);
        } else {
            children.insert(children.begin() + index, newNode);
        }
        return true;
    }
    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"]) {
            if (patchInsertNode(child, parentId, index, newNode)) return true;
        }
    }
    if (node.contains("fallback") && node["fallback"].is_object()) {
        if (patchInsertNode(node["fallback"], parentId, index, newNode)) return true;
    }
    return false;
}

static bool patchRemoveNode(nlohmann::json& node, const std::string& targetId, nlohmann::json* outRemoved = nullptr) {
    if (!node.is_object()) return false;
    if (node.contains("children") && node["children"].is_array()) {
        auto& children = node["children"];
        for (auto it = children.begin(); it != children.end(); ++it) {
            if (jsonStr(*it, "id", "") == targetId) {
                if (outRemoved) *outRemoved = *it;
                children.erase(it);
                return true;
            }
            if (patchRemoveNode(*it, targetId, outRemoved)) return true;
        }
    }
    if (node.contains("fallback") && node["fallback"].is_object()) {
        if (patchRemoveNode(node["fallback"], targetId, outRemoved)) return true;
    }
    return false;
}

static bool patchReplaceNode(nlohmann::json& node, const std::string& targetId, const nlohmann::json& replacement) {
    if (!node.is_object()) return false;
    if (jsonStr(node, "id", "") == targetId) {
        node = replacement;
        return true;
    }
    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"]) {
            if (patchReplaceNode(child, targetId, replacement)) return true;
        }
    }
    if (node.contains("fallback") && node["fallback"].is_object()) {
        if (patchReplaceNode(node["fallback"], targetId, replacement)) return true;
    }
    return false;
}

bool DeckState::handleUiSessionPatched(const std::string& sessionId, const nlohmann::json& patch) {
    std::string wid;
    auto it = sessionToWidget_.find(sessionId);
    if (it != sessionToWidget_.end()) {
        wid = it->second;
    } else {
        for (const auto& b : buttons_) {
            if (b.sessionId == sessionId) {
                wid = b.widgetId;
                sessionToWidget_[sessionId] = wid;
                break;
            }
        }
    }
    if (wid.empty()) return false;

    DeckButton* targetBtn = getButtonById(wid);
    if (!targetBtn) {
        for (auto& f : folders_) {
            for (auto& w : f.widgets) {
                if (w.widgetId == wid) {
                    targetBtn = &w;
                    break;
                }
            }
            if (targetBtn) break;
        }
    }
    if (!targetBtn || targetBtn->uiTree.is_null() || !targetBtn->uiTree.is_object()) return false;

    int fromRev = jsonInt(patch, "fromRevision", jsonInt(patch, "baseRevision", -1));
    int toRev = jsonInt(patch, "toRevision", jsonInt(patch, "nextRevision", targetBtn->revision + 1));
    if (targetBtn->sessionId != sessionId) return false;
    if (toRev <= targetBtn->revision) return true;
    if (fromRev != -1 && fromRev != targetBtn->revision) return false;

    if (!patch.contains("operations") || !patch["operations"].is_array()) return false;
    auto updatedTree = targetBtn->uiTree;
    for (const auto& op : patch["operations"]) {
        if (!op.is_object()) return false;
        std::string type = jsonStr(op, "op", jsonStr(op, "type", ""));
        if (type == "set-properties" || type == "updateProperties") {
            std::string nodeId = jsonStr(op, "nodeId", "");
            nlohmann::json props = op.contains("properties") ? op["properties"] : nlohmann::json::object();
            nlohmann::json removed = op.contains("removedProperties") ? op["removedProperties"] : nlohmann::json::array();
            if (!patchSetProperties(updatedTree, nodeId, props, removed)) return false;
        } else if (type == "insert-node") {
            std::string parentId = jsonStr(op, "parentId", "");
            int idx = jsonInt(op, "index", -1);
            if (!op.contains("node") || !patchInsertNode(updatedTree, parentId, idx, op["node"])) return false;
        } else if (type == "remove-node") {
            std::string nodeId = jsonStr(op, "nodeId", "");
            if (!patchRemoveNode(updatedTree, nodeId, nullptr)) return false;
        } else if (type == "replace-node" || type == "replaceNode") {
            std::string nodeId = jsonStr(op, "nodeId", "");
            if (!op.contains("node") || !patchReplaceNode(updatedTree, nodeId, op["node"])) return false;
        } else if (type == "move-node") {
            std::string nodeId = jsonStr(op, "nodeId", "");
            std::string parentId = jsonStr(op, "parentId", "");
            int idx = jsonInt(op, "index", -1);
            nlohmann::json removedNode;
            if (!patchRemoveNode(updatedTree, nodeId, &removedNode)) return false;
            if (!patchInsertNode(updatedTree, parentId, idx, removedNode)) return false;
        } else {
            return false;
        }
    }

    targetBtn->uiTree = std::move(updatedTree);
    targetBtn->revision = toRev;
    extractStateFromUiTree(*targetBtn);

    // Sync patched uiTree and extracted state to all matching instances in folders_
    for (auto& f : folders_) {
        for (auto& w : f.widgets) {
            if (w.widgetId == wid && &w != targetBtn) {
                w.revision = toRev;
                w.uiTree = targetBtn->uiTree;
                extractStateFromUiTree(w);
            }
        }
    }
    return true;
}

void DeckState::removeCachedResource(const std::string& key) {
    const auto entry = resourceCache_.find(key);
    if (entry == resourceCache_.end()) return;
    const auto releaseBorrow = [&](DeckButton& button) {
        if (button.iconTexture == entry->second.texture) {
            button.iconTexture = nullptr;
            button.iconWidth = button.iconHeight = 0;
            button.loadedResourceId.clear();
        }
    };
    for (auto& button : buttons_) releaseBorrow(button);
    for (auto& folder : folders_) {
        for (auto& widget : folder.widgets) releaseBorrow(widget);
    }
    if (entry->second.frames.empty()) SDL_DestroyTexture(entry->second.texture);
    resourceCacheBytes_ -= entry->second.bytes;
    resourceCache_.erase(entry);
}

bool DeckState::cacheResource(const std::string& key, SDL_Texture* texture, int width, int height, size_t hash,
                              std::vector<ArtworkFrame> frames, uint32_t loops) {
    if (!texture) return false;
    constexpr size_t cacheBudget = 24 * 1024 * 1024;
    const size_t bytes = static_cast<size_t>(width) * height * 4 * std::max<size_t>(1, frames.size());
    removeCachedResource(key);
    if (bytes > cacheBudget) {
        if (frames.empty()) SDL_DestroyTexture(texture);
        return false;
    }
    if (resourceCacheBytes_ > cacheBudget - bytes) {
        std::set<std::string> visible;
        for (const auto& button : buttons_) {
            visible.insert(button.resourceId);
            if (!button.iconBase64.empty()) visible.insert("b64:" + button.widgetId);
            visible.insert(button.treeResourceIds.begin(), button.treeResourceIds.end());
        }
        while (resourceCacheBytes_ > cacheBudget - bytes) {
            auto oldest = resourceCache_.end();
            for (auto it = resourceCache_.begin(); it != resourceCache_.end(); ++it) {
                if (!visible.count(it->first) &&
                    (oldest == resourceCache_.end() || it->second.insertionOrder < oldest->second.insertionOrder)) {
                    oldest = it;
                }
            }
            if (oldest == resourceCache_.end()) {
                if (frames.empty()) SDL_DestroyTexture(texture);
                return false;
            }
            removeCachedResource(oldest->first);
        }
    }
    resourceCache_.emplace(key, CachedResource{texture, width, height, hash, bytes, nextResourceOrder_++,
                                               std::move(frames), SDL_GetTicks64(), loops});
    resourceCacheBytes_ += bytes;
    return true;
}

void DeckState::bindResourceIcon(DeckButton& btn) {
    btn.iconTexture = nullptr;
    btn.iconWidth = btn.iconHeight = 0;
    btn.loadedResourceId.clear();
    if (!btn.resourceId.empty()) {
        btn.iconTexture = getResourceTexture(btn.resourceId, &btn.iconWidth, &btn.iconHeight);
        if (btn.iconTexture) btn.loadedResourceId = btn.resourceId;
    }
}

void DeckState::bindButtonIcon(DeckButton& btn, SDL_Renderer* renderer) {
    bindResourceIcon(btn);
    if (btn.iconTexture || btn.iconBase64.empty() || !renderer) return;

    const std::string key = "b64:" + btn.widgetId;
    const auto hash = std::hash<std::string>{}(btn.iconBase64);
    const auto cached = resourceCache_.find(key);
    if (cached != resourceCache_.end() && cached->second.contentHash == hash) {
        btn.iconTexture = cached->second.texture;
        btn.iconWidth = cached->second.width;
        btn.iconHeight = cached->second.height;
        return;
    }

    auto* texture = createTextureFromBase64(btn.iconBase64, renderer, btn.iconWidth, btn.iconHeight);
    if (cacheResource(key, texture, btn.iconWidth, btn.iconHeight, hash)) btn.iconTexture = texture;
}

bool DeckState::cacheAnimatedWebP(const std::string& id, const std::string& bytes, SDL_Renderer* renderer) {
    if (bytes.size() < 16 || bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WEBP") != 0) return false;
    const WebPData data{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demux(WebPDemux(&data), WebPDemuxDelete);
    if (!demux) return false;
    const auto count = WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT);
    const auto width = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH);
    const auto height = WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT);
    if (count < 2 || count > 90 || width == 0 || width > 512 || height == 0 || height > 512 ||
        static_cast<size_t>(width) * height * count * 4 > 3 * 1024 * 1024) return false;

    WebPAnimDecoderOptions options{};
    if (!WebPAnimDecoderOptionsInit(&options)) return false;
    options.color_mode = MODE_RGBA;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
        WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete);
    if (!decoder) return false;
    std::vector<ArtworkFrame> frames;
    frames.reserve(count);
    Uint64 previousEnd = 0;
    int previousTimestamp = 0;
    while (WebPAnimDecoderHasMoreFrames(decoder.get()) && frames.size() < count) {
        uint8_t* pixels = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(decoder.get(), &pixels, &timestamp)) return false;
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> frame(
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height),
            SDL_DestroyTexture);
        if (!frame || SDL_UpdateTexture(frame.get(), nullptr, pixels, width * 4) != 0) return false;
        SDL_SetTextureBlendMode(frame.get(), SDL_BLENDMODE_BLEND);
        const int delay = timestamp - previousTimestamp;
        previousEnd += delay <= 10 ? 100 : delay;
        previousTimestamp = timestamp;
        frames.push_back({std::move(frame), previousEnd});
    }
    if (frames.size() != count) return false;
    auto* first = frames.front().texture.get();
    const auto loops = WebPDemuxGetI(demux.get(), WEBP_FF_LOOP_COUNT);
    return cacheResource(id, first, width, height, 0, std::move(frames), loops);
}

void DeckState::setWidgetImageBytes(const std::string& widgetId, const std::string& resourceId, const std::string& rawBytes, SDL_Renderer* renderer) {
    if (!renderer || rawBytes.empty() || resourceId.empty()) return;
    const auto needsImage = [&](const DeckButton& btn) {
        return btn.resourceId == resourceId ||
               std::find(btn.treeResourceIds.begin(), btn.treeResourceIds.end(), resourceId) != btn.treeResourceIds.end();
    };
    if (std::none_of(buttons_.begin(), buttons_.end(), needsImage)) return;

    if (!getResourceTexture(resourceId) && !cacheAnimatedWebP(resourceId, rawBytes, renderer) &&
        !cacheAnimatedGif(resourceId, rawBytes, renderer)) {
        int width = 0;
        int height = 0;
        const auto* bytes = reinterpret_cast<const uint8_t*>(rawBytes.data());
        auto* texture = createTextureFromBytes(bytes, rawBytes.size(), renderer, width, height);
        if (!cacheResource(resourceId, texture, width, height)) return;
    }
    for (auto& button : buttons_) {
        if (button.resourceId == resourceId) bindResourceIcon(button);
    }
    (void)widgetId;
}

SDL_Texture* DeckState::getResourceTexture(const std::string& resourceId, int* outW, int* outH, Uint64 now) const {
    if (resourceId.empty()) return nullptr;
    auto it = resourceCache_.find(resourceId);
    if (it != resourceCache_.end() && it->second.texture) {
        if (outW) *outW = it->second.width;
        if (outH) *outH = it->second.height;
        const auto& animation = it->second;
        if (now > animation.animationStart && !animation.frames.empty()) {
            const auto elapsed = now - animation.animationStart;
            const auto period = animation.frames.back().endsAt;
            if (animation.loops != 0 && elapsed / period >= animation.loops) return animation.frames.back().texture.get();
            const auto phase = elapsed % period;
            const auto frame = std::upper_bound(animation.frames.begin(), animation.frames.end(), phase,
                [](Uint64 ticks, const ArtworkFrame& entry) { return ticks < entry.endsAt; });
            return frame->texture.get();
        }
        return animation.texture;
    }
    return nullptr;
}

std::vector<std::pair<std::string, std::string>> DeckState::getPendingResourceRequests() const {
    std::vector<std::pair<std::string, std::string>> reqs;
    for (const auto& b : buttons_) {
        if (b.widgetId.empty()) continue;
        if (!b.resourceId.empty()) {
            if (!resourceCache_.count(b.resourceId) || !resourceCache_.at(b.resourceId).texture) {
                if (b.resourceId != b.loadedResourceId) {
                    reqs.push_back({b.widgetId, b.resourceId});
                }
            }
        }
        for (const auto& rid : b.treeResourceIds) {
            if (rid.empty() || rid == b.resourceId) continue;
            if (!resourceCache_.count(rid) || !resourceCache_.at(rid).texture) {
                reqs.push_back({b.widgetId, rid});
            }
        }
    }
    return reqs;
}

// ============================================================================
// Folder & Page Navigation Helpers
// ============================================================================

bool DeckState::openFolder(const std::string& folderId, SDL_Renderer* renderer) {
    if (folderId.empty() || folderId == currentFolderId_) return false;
    for (const auto& f : folders_) {
        if (f.id == folderId || f.name == folderId) {
            if (!currentFolderId_.empty()) {
                folderHistory_.push_back(currentFolderId_);
            }
            currentFolderId_ = f.id;
            syncActiveFolderToButtons(renderer);
            return true;
        }
    }
    return false;
}

bool DeckState::goParentFolder(SDL_Renderer* renderer) {
    for (const auto& f : folders_) {
        if (f.id == currentFolderId_ && !f.parentId.empty()) {
            std::string parent = f.parentId;
            for (const auto& pf : folders_) {
                if (pf.id == parent || pf.name == parent) {
                    folderHistory_.clear();
                    currentFolderId_ = pf.id;
                    syncActiveFolderToButtons(renderer);
                    return true;
                }
            }
        }
    }
    return false;
}

bool DeckState::goBackFolder(SDL_Renderer* renderer) {
    return goParentFolder(renderer);
}

static bool belongsToProfile(const DeckFolder& folder, const std::string& profileId) {
    return profileId.empty() || folder.profileId.empty() || folder.profileId == profileId;
}

std::vector<FolderSummary> DeckState::getActiveProfileFolders() const {
    std::vector<FolderSummary> active;
    for (const auto& f : folders_) {
        if (belongsToProfile(f, currentProfileId_)) {
            active.push_back({f.id, f.name, f.isDefault});
        }
    }
    return active;
}

int DeckState::getFolderCount() const {
    return static_cast<int>(std::count_if(folders_.begin(), folders_.end(), [&](const DeckFolder& folder) {
        return belongsToProfile(folder, currentProfileId_);
    }));
}

bool DeckState::switchProfile(const std::string& profileId, SDL_Renderer* renderer) {
    if (profileId.empty() || profileId == currentProfileId_) return false;
    const auto profile = std::find_if(profiles_.begin(), profiles_.end(), [&](const ProfileInfo& item) {
        return item.id == profileId;
    });
    if (profile == profiles_.end()) return false;
    currentProfileId_ = profile->id;
    currentProfileName_ = profile->name;
    auto active = getActiveProfileFolders();
    std::string defFolderId = active.empty() ? "" : active[0].id;
    for (const auto& f : active) {
        if (f.isDefault) {
            defFolderId = f.id;
            break;
        }
    }
    currentFolderId_ = defFolderId;
    folderHistory_.clear();
    syncActiveFolderToButtons(renderer);
    return true;
}

bool DeckState::goHomeFolder(SDL_Renderer* renderer) {
    auto active = getActiveProfileFolders();
    if (active.empty()) return false;
    std::string homeId = active[0].id;
    for (const auto& f : active) {
        if (f.isDefault) {
            homeId = f.id;
            break;
        }
    }
    return openFolder(homeId, renderer);
}

bool DeckState::nextFolder(SDL_Renderer* renderer) {
    auto active = getActiveProfileFolders();
    if (active.size() <= 1) return false;
    int idx = getCurrentFolderIndex();
    int nextIdx = (idx + 1) % static_cast<int>(active.size());
    return openFolder(active[nextIdx].id, renderer);
}

bool DeckState::prevFolder(SDL_Renderer* renderer) {
    auto active = getActiveProfileFolders();
    if (active.size() <= 1) return false;
    int idx = getCurrentFolderIndex();
    int prevIdx = (idx - 1 + static_cast<int>(active.size())) % static_cast<int>(active.size());
    return openFolder(active[prevIdx].id, renderer);
}

int DeckState::getCurrentFolderIndex() const {
    int index = 0;
    for (const auto& folder : folders_) {
        if (!belongsToProfile(folder, currentProfileId_)) continue;
        if (folder.id == currentFolderId_) return index;
        ++index;
    }
    return 0;
}

bool DeckState::canGoBackOrParent() const {
    for (const auto& f : folders_) {
        if (f.id == currentFolderId_ && !f.parentId.empty()) return true;
    }
    return false;
}

std::string DeckState::getCurrentFolderName() const {
    for (const auto& f : folders_) {
        if (f.id == currentFolderId_) return f.name;
    }
    return "Home";
}

DeckButton* DeckState::getButton(int y, int x) {
    for (auto& b : buttons_) {
        if (x >= b.posX && x < b.posX + b.width &&
            y >= b.posY && y < b.posY + b.height) {
            return &b;
        }
    }
    return nullptr;
}

const DeckButton* DeckState::getButtonConst(int y, int x) const {
    for (const auto& b : buttons_) {
        if (x >= b.posX && x < b.posX + b.width &&
            y >= b.posY && y < b.posY + b.height) {
            return &b;
        }
    }
    return nullptr;
}

DeckButton* DeckState::getButtonById(const std::string& widgetId) {
    if (widgetId.empty()) return nullptr;
    for (auto& b : buttons_) {
        if (b.widgetId == widgetId) return &b;
    }
    return nullptr;
}

std::vector<std::string> DeckState::getDisplayedWidgetIds() const {
    std::vector<std::string> ids;
    for (const auto& b : buttons_) {
        if (!b.widgetId.empty()) ids.push_back(b.widgetId);
    }
    return ids;
}

void DeckState::setButtonState(int y, int x, bool pressed, int subBtnIdx) {
    DeckButton* btn = getButton(y, x);
    if (btn) {
        btn->isPressed = pressed;
        if (pressed) btn->pressFeedbackSeconds = 0.060f;
        btn->activeSubBtnIndex = pressed ? subBtnIdx : -1;
    }
}

void DeckState::updateFolderLayoutLocally(const std::string& folderId, int rows, int cols, int spacing, int radius, SDL_Renderer* renderer) {
    std::string targetId = folderId.empty() ? currentFolderId_ : folderId;
    for (auto& f : folders_) {
        if (f.id == targetId) {
            if (rows > 0) f.rows = rows;
            if (cols > 0) f.cols = cols;
            if (spacing >= 0) f.widgetSpacing = spacing;
            if (radius >= 0) f.widgetBorderRadius = radius;
            break;
        }
    }
    if (targetId == currentFolderId_) {
        if (rows > 0) rows_ = rows;
        if (cols > 0) cols_ = cols;
        if (spacing >= 0) spacing_ = spacing;
        if (radius >= 0) radius_ = radius;
        syncActiveFolderToButtons(renderer);
    }
}

void DeckState::updateAnimations(Uint64 nowTicks, float deltaSec) {
    for (auto& b : buttons_) {
        b.updateAnimations(nowTicks, deltaSec);
    }
}
