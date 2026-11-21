#include "ui_renderer.hpp"
#include "widget_localization.hpp"
#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL2/SDL_image.h>
#include <iostream>
#include <cmath>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <vector>

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;
static const int STATUS_BAR_H = 52;

namespace {
struct TextCacheKey {
    TTF_Font* font;
    std::string text;
    bool operator==(const TextCacheKey& o) const {
        return font == o.font && text == o.text;
    }
};

struct TextCacheKeyHash {
    std::size_t operator()(const TextCacheKey& k) const {
        std::size_t h1 = std::hash<const void*>{}(k.font);
        std::size_t h2 = std::hash<std::string>{}(k.text);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

struct CachedTextEntry {
    SDL_Texture* tex = nullptr;
    int w = 0;
    int h = 0;
};

static std::unordered_map<TextCacheKey, CachedTextEntry, TextCacheKeyHash> g_textCache;
static std::unordered_map<int, SDL_Texture*> g_cornerMaskCache;
} // namespace

void UIRenderer::clearRenderCaches() {
    for (auto& kv : g_textCache) {
        if (kv.second.tex) {
            SDL_DestroyTexture(kv.second.tex);
        }
    }
    g_textCache.clear();

    for (auto& kv : g_cornerMaskCache) {
        if (kv.second) {
            SDL_DestroyTexture(kv.second);
        }
    }
    g_cornerMaskCache.clear();
}

SDL_Texture* UIRenderer::getCachedTextTexture(SDL_Renderer* r, TTF_Font* font, const std::string& text, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!r || !font || text.empty()) return nullptr;

    TextCacheKey key{font, text};
    auto it = g_textCache.find(key);
    if (it != g_textCache.end() && it->second.tex) {
        outW = it->second.w;
        outH = it->second.h;
        return it->second.tex;
    }

    if (g_textCache.size() >= 640) {
        for (auto& kv : g_textCache) {
            if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        }
        g_textCache.clear();
    }

    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), SDL_Color{255, 255, 255, 255});
    if (!surf) return nullptr;

    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    int w = surf->w;
    int h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    g_textCache.emplace(std::move(key), CachedTextEntry{tex, w, h});
    outW = w;
    outH = h;
    return tex;
}

UIRenderer::UIRenderer() {}

UIRenderer::~UIRenderer() {
    shutdown();
}

bool UIRenderer::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) < 0) {
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    window_ = SDL_CreateWindow("SwitchDeck",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    if (!window_) return false;

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) return false;

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (TTF_Init() == -1) {
        return false;
    }

    const char* fontPaths[] = {
        "romfs:/font.ttf",
        "sdmc:/switch/SwitchDeck/font.ttf"
    };

    for (const char* path : fontPaths) {
        fontTiny_   = TTF_OpenFont(path, 13);
        fontSmall_  = TTF_OpenFont(path, 15);
        fontNormal_ = TTF_OpenFont(path, 18);
        fontMedium_ = TTF_OpenFont(path, 22);
        fontLarge_  = TTF_OpenFont(path, 28);
        fontXl_     = TTF_OpenFont(path, 34);
        fontXxl_    = TTF_OpenFont(path, 44);
        if (fontNormal_) {
            widgetFontPath_ = path;
            wordmarkFont_ = TTF_OpenFont(path, 25);
            if (wordmarkFont_) TTF_SetFontStyle(wordmarkFont_, TTF_STYLE_BOLD);
            break;
        }
    }

    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_JoystickOpen(i);
    }

    return true;
}

void UIRenderer::shutdown() {
    settingsUI_.releaseTextures();
    clearRenderCaches();
    for (auto& entry : widgetFonts_) TTF_CloseFont(entry.second);
    widgetFonts_.clear();
    artworkTransitions_.clear();
    if (wordmarkFont_) { TTF_CloseFont(wordmarkFont_); wordmarkFont_ = nullptr; }
    IMG_Quit();

    if (fontTiny_)   { TTF_CloseFont(fontTiny_);   fontTiny_ = nullptr; }
    if (fontSmall_)  { TTF_CloseFont(fontSmall_);  fontSmall_ = nullptr; }
    if (fontNormal_) { TTF_CloseFont(fontNormal_); fontNormal_ = nullptr; }
    if (fontMedium_) { TTF_CloseFont(fontMedium_); fontMedium_ = nullptr; }
    if (fontLarge_)  { TTF_CloseFont(fontLarge_);  fontLarge_ = nullptr; }
    if (fontXl_)     { TTF_CloseFont(fontXl_);     fontXl_ = nullptr; }
    if (fontXxl_)    { TTF_CloseFont(fontXxl_);    fontXxl_ = nullptr; }
    TTF_Quit();

    if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
    if (window_)   { SDL_DestroyWindow(window_);     window_ = nullptr; }
    SDL_Quit();
}

// ============================================================================
// Anti-Aliased Geometry Helpers (SDL2_gfx + Hardware Corner Mask Cache)
// ============================================================================

void UIRenderer::drawRoundedBox(SDL_Renderer* r, const SDL_Rect& rect, int radius, SDL_Color color) {
    if (!r || rect.w <= 0 || rect.h <= 0 || color.a == 0) return;
    int maxRad = std::max(0, (std::min(rect.w, rect.h) - 2) / 2);
    int rad = std::clamp(radius, 0, maxRad);
    Sint16 x1 = static_cast<Sint16>(rect.x);
    Sint16 y1 = static_cast<Sint16>(rect.y);
    Sint16 x2 = static_cast<Sint16>(rect.x + rect.w - 1);
    Sint16 y2 = static_cast<Sint16>(rect.y + rect.h - 1);

    if (rad <= 1) {
        boxRGBA(r, x1, y1, x2, y2, color.r, color.g, color.b, color.a);
    } else {
        roundedBoxRGBA(r, x1, y1, x2, y2, static_cast<Sint16>(rad), color.r, color.g, color.b, color.a);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

void UIRenderer::drawRoundedBorder(SDL_Renderer* r, const SDL_Rect& rect, int radius, int thickness, SDL_Color color) {
    if (!r || rect.w <= 0 || rect.h <= 0 || color.a == 0 || thickness <= 0) return;
    for (int i = 0; i < thickness; ++i) {
        SDL_Rect cur = { rect.x + i, rect.y + i, rect.w - 2 * i, rect.h - 2 * i };
        if (cur.w <= 0 || cur.h <= 0) break;
        int maxRad = std::max(0, (std::min(cur.w, cur.h) - 2) / 2);
        int rad = std::clamp(radius - i, 0, maxRad);
        if (rad <= 1) {
            // SDL draws the outline from the same pixel bounds as the tile.
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
            SDL_RenderDrawRect(r, &cur);
        } else {
            const auto x1 = static_cast<Sint16>(cur.x);
            const auto y1 = static_cast<Sint16>(cur.y);
            const auto x2 = static_cast<Sint16>(cur.x + cur.w - 1);
            const auto y2 = static_cast<Sint16>(cur.y + cur.h - 1);
            roundedRectangleRGBA(r, x1, y1, x2, y2, static_cast<Sint16>(rad), color.r, color.g, color.b, color.a);
        }
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

static SDL_Texture* getOrCreateCornerMaskTexture(SDL_Renderer* r, int rad) {
    if (!r || rad <= 1) return nullptr;
    auto it = g_cornerMaskCache.find(rad);
    if (it != g_cornerMaskCache.end() && it->second) {
        return it->second;
    }

    int dim = rad * 2;
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, dim, dim, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return nullptr;

    SDL_LockSurface(surf);
    Uint8* pixels = static_cast<Uint8*>(surf->pixels);
    float rf = static_cast<float>(rad);
    for (int y = 0; y < dim; ++y) {
        float dy = (y < rad) ? (rf - (y + 0.5f)) : ((y - rad) + 0.5f);
        for (int x = 0; x < dim; ++x) {
            float dx = (x < rad) ? (rf - (x + 0.5f)) : ((x - rad) + 0.5f);
            float dist = std::sqrt(dx * dx + dy * dy);
            float cov = std::clamp(dist - (rf - 0.5f), 0.0f, 1.0f);
            Uint8* p = pixels + y * surf->pitch + x * 4;
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
            p[3] = static_cast<Uint8>(std::round(cov * 255.0f));
        }
    }
    SDL_UnlockSurface(surf);

    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    g_cornerMaskCache[rad] = tex;
    return tex;
}

void UIRenderer::maskRoundedCorners(SDL_Renderer* r, const SDL_Rect& rect, int radius, SDL_Color outerBg) {
    if (!r || rect.w <= 4 || rect.h <= 4 || radius <= 1) return;
    int maxRad = std::max(0, (std::min(rect.w, rect.h) - 2) / 2);
    int rad = std::clamp(radius, 0, maxRad);
    if (rad <= 1) return;

    SDL_Texture* maskTex = getOrCreateCornerMaskTexture(r, rad);
    if (!maskTex) return;

    SDL_SetTextureColorMod(maskTex, outerBg.r, outerBg.g, outerBg.b);
    SDL_SetTextureAlphaMod(maskTex, outerBg.a);

    SDL_Rect srcTL = {0, 0, rad, rad};
    SDL_Rect srcTR = {rad, 0, rad, rad};
    SDL_Rect srcBL = {0, rad, rad, rad};
    SDL_Rect srcBR = {rad, rad, rad, rad};

    SDL_Rect dstTL = {rect.x, rect.y, rad, rad};
    SDL_Rect dstTR = {rect.x + rect.w - rad, rect.y, rad, rad};
    SDL_Rect dstBL = {rect.x, rect.y + rect.h - rad, rad, rad};
    SDL_Rect dstBR = {rect.x + rect.w - rad, rect.y + rect.h - rad, rad, rad};

    SDL_RenderCopy(r, maskTex, &srcTL, &dstTL);
    SDL_RenderCopy(r, maskTex, &srcTR, &dstTR);
    SDL_RenderCopy(r, maskTex, &srcBL, &dstBL);
    SDL_RenderCopy(r, maskTex, &srcBR, &dstBR);
}

void UIRenderer::drawFilledCircleAA(SDL_Renderer* r, int cx, int cy, int radius, SDL_Color fill, SDL_Color border, int borderThickness) {
    if (!r || radius <= 0) return;
    if (borderThickness > 0 && border.a > 0) {
        filledCircleRGBA(r, static_cast<Sint16>(cx), static_cast<Sint16>(cy), static_cast<Sint16>(radius), border.r, border.g, border.b, border.a);
        aacircleRGBA(r, static_cast<Sint16>(cx), static_cast<Sint16>(cy), static_cast<Sint16>(radius), border.r, border.g, border.b, border.a);
        int innerR = std::max(1, radius - borderThickness);
        filledCircleRGBA(r, static_cast<Sint16>(cx), static_cast<Sint16>(cy), static_cast<Sint16>(innerR), fill.r, fill.g, fill.b, fill.a);
        aacircleRGBA(r, static_cast<Sint16>(cx), static_cast<Sint16>(cy), static_cast<Sint16>(innerR), fill.r, fill.g, fill.b, fill.a);
    } else {
        filledCircleRGBA(r, static_cast<Sint16>(cx), static_cast<Sint16>(cy), static_cast<Sint16>(radius), fill.r, fill.g, fill.b, fill.a);
        aacircleRGBA(r, static_cast<Sint16>(cx), static_cast<Sint16>(cy), static_cast<Sint16>(radius), fill.r, fill.g, fill.b, fill.a);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

// ============================================================================
// UTF-8 Safe Typography Helpers (Hardware Texture Cached)
// ============================================================================

static void popLastUtf8Char(std::string& s) {
    while (!s.empty()) {
        unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

int UIRenderer::getTextWidth(TTF_Font* font, const std::string& text) const {
    if (!font || text.empty()) return 0;
    TextCacheKey key{font, text};
    auto it = g_textCache.find(key);
    if (it != g_textCache.end()) {
        return it->second.w;
    }
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text.c_str(), &w, &h) == 0) return w;
    return 0;
}

std::string UIRenderer::ellipsizeText(TTF_Font* font, const std::string& text, int maxWidth) const {
    if (!font || text.empty() || maxWidth <= 0) return text;
    if (getTextWidth(font, text) <= maxWidth) return text;

    std::string trimmed = text;
    while (!trimmed.empty()) {
        popLastUtf8Char(trimmed);
        while (!trimmed.empty() && trimmed.back() == ' ') {
            trimmed.pop_back();
        }
        std::string candidate = trimmed + "...";
        if (getTextWidth(font, candidate) <= maxWidth) {
            return candidate;
        }
    }
    return "...";
}

void UIRenderer::drawText(TTF_Font* font, const std::string& text, int x, int y, SDL_Color color, bool center, int maxWidth, bool rightAlign, bool dropShadow, bool vCenter) {
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

void UIRenderer::drawWrappedText(TTF_Font* font, const std::string& text, const SDL_Rect& area, const std::string& vPos, const std::string& hAlign, SDL_Color color, bool dropShadow) {
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

// ============================================================================
// Square Grid Geometry (Exact 1:1 Macro Deck 3 GridMetrics Algorithm)
// ============================================================================

SDL_Rect UIRenderer::getCellRect(int row, int col, int spanW, int spanH, const DeckState& state, const AppConfig& config) const {
    int rows = std::max(1, state.getRows());
    int cols = std::max(1, state.getCols());
    int spacing = std::clamp(state.getSpacing(), 0, 48);

    const float WIDGET_REFERENCE_CELL_SIZE = 120.0f;
    const float ratio = spacing / WIDGET_REFERENCE_CELL_SIZE;

    int outerMargin = 16;
    int padTop = config.show_status_bar ? (STATUS_BAR_H + outerMargin) : 48;
    int padBottom = outerMargin;

    float availW = static_cast<float>(SCREEN_W - 2 * outerMargin);
    float availH = static_cast<float>(SCREEN_H - padTop - padBottom);

    float cellFromWidth = availW / (cols + (cols - 1) * ratio + 2 * ratio);
    float cellFromHeight = availH / (rows + (rows - 1) * ratio + 2 * ratio);
    float cellSize = std::max(1.0f, std::min(cellFromWidth, cellFromHeight));

    float gap = cellSize * ratio;
    float padding = cellSize * ratio;

    float solvedWidth = cols * cellSize + (cols - 1) * gap + 2 * padding;
    float solvedHeight = rows * cellSize + (rows - 1) * gap + 2 * padding;

    float startX = outerMargin + (availW - solvedWidth) / 2.0f;
    float startY = padTop + (availH - solvedHeight) / 2.0f;

    int x = static_cast<int>(std::round(startX + padding + col * (cellSize + gap)));
    int y = static_cast<int>(std::round(startY + padding + row * (cellSize + gap)));
    int w = static_cast<int>(std::round(spanW * cellSize + (spanW - 1) * gap));
    int h = static_cast<int>(std::round(spanH * cellSize + (spanH - 1) * gap));

    return { x, y, w, h };
}

int UIRenderer::getTileCornerRadius(int cellSize, const DeckState& state, const AppConfig& config) const {
    (void)config;
    int rawRadius = state.getRadius();
    if (rawRadius <= 0) return 0;
    const int scaledRadius = static_cast<int>(std::lround(rawRadius * (cellSize / 120.0)));
    return std::clamp(scaledRadius, 0, cellSize / 2);
}

bool UIRenderer::getButtonAt(int screenX, int screenY, const DeckState& state, const AppConfig& config, int& outRow, int& outCol) const {
    for (const auto& btn : state.getButtons()) {
        SDL_Rect r = getCellRect(btn.posY, btn.posX, btn.width, btn.height, state, config);
        if (screenX >= r.x && screenX <= r.x + r.w &&
            screenY >= r.y && screenY <= r.y + r.h) {
            outRow = btn.posY;
            outCol = btn.posX;
            return true;
        }
    }

    int rows = std::max(1, state.getRows());
    int cols = std::max(1, state.getCols());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            SDL_Rect rect = getCellRect(r, c, 1, 1, state, config);
            if (screenX >= rect.x && screenX <= rect.x + rect.w &&
                screenY >= rect.y && screenY <= rect.y + rect.h) {
                outRow = r;
                outCol = c;
                return true;
            }
        }
    }
    return false;
}

float UIRenderer::getSliderLevelAt(int screenX, int screenY, const DeckButton& btn, const DeckState& state, const AppConfig& config) const {
    SDL_Rect r = getCellRect(btn.posY, btn.posX, btn.width, btn.height, state, config);
    if (btn.sliderIsVertical) {
        int topPad = (btn.sliderShowLabel && !btn.labelText.empty()) ? 34 : 16;
        int bottomPad = btn.sliderShowValue ? 36 : 16;
        int trackY = r.y + topPad;
        int trackH = std::max(20, r.h - topPad - bottomPad);
        float distFromBottom = static_cast<float>((trackY + trackH) - screenY);
        float lvl = distFromBottom / static_cast<float>(trackH);
        return std::clamp(lvl, 0.0f, 1.0f);
    } else {
        int padX = 20;
        int trackX = r.x + padX;
        int trackW = std::max(20, r.w - 2 * padX);
        float distFromLeft = static_cast<float>(screenX - trackX);
        float lvl = distFromLeft / static_cast<float>(trackW);
        return std::clamp(lvl, 0.0f, 1.0f);
    }
}

int UIRenderer::getMusicSubButtonAt(int screenX, int screenY, const DeckButton& btn, const DeckState& state, const AppConfig& config) const {
    if (btn.subButtons.empty()) return -1;
    SDL_Rect r = getCellRect(btn.posY, btn.posX, btn.width, btn.height, state, config);
    int count = static_cast<int>(btn.subButtons.size());
    int sliceW = std::max(1, r.w / count);
    int idx = (screenX - r.x) / sliceW;
    return std::clamp(idx, 0, count - 1);
}

bool UIRenderer::isSettingsButtonTouched(int screenX, int screenY, const AppConfig& config) const {
    if (config.show_status_bar) {
        return (screenX >= rectBarSettings_.x && screenX <= rectBarSettings_.x + rectBarSettings_.w &&
                screenY >= rectBarSettings_.y && screenY <= rectBarSettings_.y + rectBarSettings_.h);
    }
    return (screenX >= rectFloatingSettings_.x - 8 && screenX <= rectFloatingSettings_.x + rectFloatingSettings_.w + 8 &&
            screenY >= rectFloatingSettings_.y - 8 && screenY <= rectFloatingSettings_.y + rectFloatingSettings_.h + 8);
}

bool UIRenderer::isBackButtonTouched(int screenX, int screenY, const DeckState& state) const {
    if (!state.canGoBackOrParent()) return false;
    return (screenX >= rectFloatingBack_.x && screenX <= rectFloatingBack_.x + rectFloatingBack_.w &&
            screenY >= rectFloatingBack_.y && screenY <= rectFloatingBack_.y + rectFloatingBack_.h);
}

bool UIRenderer::isFolderPrevTouched(int screenX, int screenY, const DeckState& state) const {
    const SDL_Point point{screenX, screenY};
    return state.getFolderCount() > 1 && SDL_PointInRect(&point, &rectBarFolderPrev_);
}

bool UIRenderer::isFolderNextTouched(int screenX, int screenY, const DeckState& state) const {
    const SDL_Point point{screenX, screenY};
    return state.getFolderCount() > 1 && SDL_PointInRect(&point, &rectBarFolderNext_);
}

bool UIRenderer::isUpdateModalDismissTouched(int screenX, int screenY) const {
    return (screenX >= rectUpdateModalDismiss_.x && screenX <= rectUpdateModalDismiss_.x + rectUpdateModalDismiss_.w &&
            screenY >= rectUpdateModalDismiss_.y && screenY <= rectUpdateModalDismiss_.y + rectUpdateModalDismiss_.h);
}

void UIRenderer::moveFocus(int dRow, int dCol, int maxRows, int maxCols) {
    if (maxRows < 1) maxRows = 1;
    if (maxCols < 1) maxCols = 1;
    showFocusHighlight_ = true;
    focusRow_ = (focusRow_ + dRow + maxRows) % maxRows;
    focusCol_ = (focusCol_ + dCol + maxCols) % maxCols;
}

void UIRenderer::setFocus(int row, int col) {
    focusRow_ = row;
    focusCol_ = col;
}

// ============================================================================
// Main Render Pass
// ============================================================================

void UIRenderer::render(const AppConfig& config,
                        const DeckState& state,
                        WsState wsState,
                        int pingMs,
                        const std::string& statusMsg,
                        bool isDiscovering,
                        const std::string& discoveryStatus,
                        int batteryPct,
                        bool isCharging,
                        int dimAlpha,
                        bool showUpdateModal,
                        const std::string& newUpdateVersion,
                        const std::string& actionError)
{
    if (dimAlpha == 255) {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        SDL_RenderPresent(renderer_);
        return;
    }

    if (settingsUI_.isOpen()) {
        settingsUI_.render(renderer_, fontTiny_, fontSmall_, fontNormal_, fontMedium_, fontLarge_,
                           wordmarkFont_ ? wordmarkFont_ : fontMedium_,
                           state, wsState, pingMs, statusMsg, isDiscovering, discoveryStatus, batteryPct, isCharging);
        if (showUpdateModal) {
            renderUpdateModal(newUpdateVersion);
        }
        if (dimAlpha > 0) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, static_cast<Uint8>(dimAlpha));
            SDL_RenderFillRect(renderer_, nullptr);
        }
        SDL_RenderPresent(renderer_);
        return;
    }

    // Canvas Background by Theme
    SDL_Color canvasBg;
    if (config.theme == 1) { // Light
        canvasBg = {232, 238, 246, 255};
    } else if (config.theme == 2) { // OLED Black
        canvasBg = {0, 0, 0, 255};
    } else { // Dark (Macro Deck 3 default #121212 or folder custom background)
        uint8_t fR = 18, fG = 18, fB = 18;
        if (state.getCurrentFolderBg(fR, fG, fB)) {
            canvasBg = {fR, fG, fB, 255};
        } else {
            canvasBg = {18, 18, 18, 255};
        }
    }
    currentCanvasBg_ = canvasBg;
    SDL_SetRenderDrawColor(renderer_, canvasBg.r, canvasBg.g, canvasBg.b, 255);
    SDL_RenderClear(renderer_);

    int rows = std::max(1, state.getRows());
    int cols = std::max(1, state.getCols());

    SDL_Rect singleCell = getCellRect(0, 0, 1, 1, state, config);
    int tileRadius = getTileCornerRadius(singleCell.w, state, config);

    std::vector<std::vector<bool>> occupied(rows, std::vector<bool>(cols, false));
    for (const auto& btn : state.getButtons()) {
        for (int r = btn.posY; r < std::min(rows, btn.posY + btn.height); ++r) {
            for (int c = btn.posX; c < std::min(cols, btn.posX + btn.width); ++c) {
                if (r >= 0 && c >= 0) occupied[r][c] = true;
            }
        }
    }

    // 1. Render Empty Cells
    bool showEmpty = (config.show_empty_cells == 1) ||
                     (config.show_empty_cells < 0 && state.isEmptyCellsVisible());
    if (showEmpty || showFocusHighlight_) {
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                if (!occupied[r][c]) {
                    bool isFocused = showFocusHighlight_ && (r == focusRow_ && c == focusCol_);
                    if (showEmpty || isFocused) {
                        SDL_Rect cellRect = getCellRect(r, c, 1, 1, state, config);
                        renderEmptyCell(cellRect, tileRadius, isFocused, config.theme);
                    }
                }
            }
        }
    }

    const auto ticks = SDL_GetTicks64();
    for (auto it = artworkTransitions_.begin(); it != artworkTransitions_.end();) {
        if (ticks - it->second.lastSeen > 1000) it = artworkTransitions_.erase(it);
        else ++it;
    }

    // 2. Render Active Widgets
    for (const auto& btn : state.getButtons()) {
        SDL_Rect rect = getCellRect(btn.posY, btn.posX, btn.width, btn.height, state, config);
        bool isFocused = showFocusHighlight_ &&
                         (focusRow_ >= btn.posY && focusRow_ < btn.posY + btn.height &&
                          focusCol_ >= btn.posX && focusCol_ < btn.posX + btn.width);
        renderWidgetTile(btn, rect, tileRadius, isFocused, state, config, batteryPct, isCharging);
    }

    // 3. Render Top Status Bar or Floating Controls
    if (config.show_status_bar) {
        renderTopStatusBar(config, state, wsState, pingMs, statusMsg, batteryPct, isCharging);
    } else {
        renderFloatingControls(state, config.language);
    }

    if (!actionError.empty()) {
        const int width = 560;
        const int y = SCREEN_H - 68;
        roundedBoxRGBA(renderer_, (SCREEN_W - width) / 2, y,
                       (SCREEN_W + width) / 2, y + 44, 10, 111, 32, 36, 255);
        drawText(fontNormal_, actionError, SCREEN_W / 2, y + 22,
                 {255, 255, 255, 255}, true, width - 24, false, false, true);
    }

    // 4. Idle Screen Dimmer / Screen-Off Overlay
    if (dimAlpha > 0) {
        int alpha = std::clamp(dimAlpha, 0, 255);
        boxRGBA(renderer_, 0, 0, SCREEN_W - 1, SCREEN_H - 1, 0, 0, 0, static_cast<Uint8>(alpha));
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    }

    // 5. Update Notification Modal (if new version detected)
    if (showUpdateModal) {
        renderUpdateModal(newUpdateVersion);
    }

    SDL_RenderPresent(renderer_);
}

void UIRenderer::renderTopStatusBar(const AppConfig& config,
                                    const DeckState& state,
                                    WsState wsState,
                                    int pingMs,
                                    const std::string& statusMsg,
                                    int batteryPct,
                                    bool isCharging)
{
    (void)statusMsg;
    bool isLight = (config.theme == 1);
    bool isOled = (config.theme == 2);

    SDL_Color barBg = isOled ? SDL_Color{10, 12, 18, 255}
                     : (isLight ? SDL_Color{255, 255, 255, 255} : SDL_Color{15, 19, 28, 255});
    SDL_Color brdCol = isOled ? SDL_Color{28, 34, 48, 255}
                      : (isLight ? SDL_Color{218, 224, 236, 255} : SDL_Color{32, 40, 56, 255});
    SDL_Color textPrimary = isLight ? SDL_Color{15, 23, 42, 255} : SDL_Color{248, 250, 252, 255};
    SDL_Color textSubtle  = isLight ? SDL_Color{100, 116, 139, 255} : SDL_Color{148, 163, 184, 255};
    SDL_Color btnBg = isOled ? SDL_Color{20, 24, 34, 255}
                     : (isLight ? SDL_Color{241, 245, 249, 255} : SDL_Color{24, 30, 44, 255});
    SDL_Color btnBrd = isOled ? SDL_Color{38, 46, 64, 255}
                      : (isLight ? SDL_Color{203, 213, 225, 255} : SDL_Color{44, 54, 76, 255});

    // 1. Status Bar Background + subtle bottom border & shadow
    SDL_Rect barRect = {0, 0, SCREEN_W, STATUS_BAR_H};
    drawRoundedBox(renderer_, barRect, 0, barBg);
    SDL_SetRenderDrawColor(renderer_, brdCol.r, brdCol.g, brdCol.b, 255);
    SDL_RenderDrawLine(renderer_, 0, STATUS_BAR_H - 1, SCREEN_W, STATUS_BAR_H - 1);
    boxRGBA(renderer_, 0, STATUS_BAR_H, SCREEN_W - 1, STATUS_BAR_H + 3, 0, 0, 0, isLight ? 16 : 60);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    int cy = STATUS_BAR_H / 2;

    drawWordmark(renderer_, wordmarkFont_ ? wordmarkFont_ : fontMedium_, fontSmall_, 20, cy, textPrimary, isLight);

    renderPageControls(state, btnBg, btnBrd, textPrimary, textSubtle);

    // 4. Right: Connection Indicator Pill (Shared)
    drawConnectionPill(renderer_, fontSmall_, fontTiny_, rectBarConn_, wsState, pingMs, isLight, isOled, config.language);

    // 5. Right: Battery % & Charging Indicator Pill (Shared)
    drawBatteryPill(renderer_, fontSmall_, rectBarBattery_, batteryPct, isCharging, config.low_battery_threshold, isLight, isOled, config.language);

    // 7. Right: Modern "Ayarlar" Button
    drawRoundedBox(renderer_, rectBarSettings_, 10, {37, 99, 235, 255});
    drawRoundedBorder(renderer_, rectBarSettings_, 10, 1, {96, 165, 250, 255});
    drawText(fontSmall_, (config.language == 1 ? "Settings" : "Ayarlar"), rectBarSettings_.x + rectBarSettings_.w / 2, rectBarSettings_.y + rectBarSettings_.h / 2, {255, 255, 255, 255}, true);
}

void UIRenderer::drawWordmark(SDL_Renderer* r, TTF_Font* titleFont, TTF_Font* signatureFont, int x, int centerY,
                              SDL_Color titleColor, bool isLight) {
    if (!titleFont) return;
    const int baseline = centerY - TTF_FontHeight(titleFont) / 2 + TTF_FontAscent(titleFont);
    const auto draw = [&](TTF_Font* font, const char* text, int left, SDL_Color color) {
        int width = 0, height = 0;
        if (auto* texture = getCachedTextTexture(r, font, text, width, height)) {
            const SDL_Rect target{left, baseline - TTF_FontAscent(font), width, height};
            SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(texture, color.a);
            SDL_RenderCopy(r, texture, nullptr, &target);
        }
        return width;
    };
    const int titleWidth = draw(titleFont, "SwitchDeck", x, titleColor);
    const SDL_Color blue = isLight ? SDL_Color{29, 78, 216, 255} : SDL_Color{96, 165, 250, 255};
    if (signatureFont) draw(signatureFont, "by SertAy", x + titleWidth + 11, blue);
}

void UIRenderer::drawConnectionPill(SDL_Renderer* r, TTF_Font* fontSmall, TTF_Font* fontTiny, const SDL_Rect& rect, WsState wsState, int pingMs, bool isLight, bool isOled, int language) {
    if (!r || rect.w <= 0 || rect.h <= 0) return;

    SDL_Color pillBg = isOled ? SDL_Color{20, 24, 34, 255}
                     : (isLight ? SDL_Color{241, 245, 249, 255} : SDL_Color{24, 30, 44, 255});
    SDL_Color pillBrd = isOled ? SDL_Color{38, 46, 64, 255}
                      : (isLight ? SDL_Color{203, 213, 225, 255} : SDL_Color{44, 54, 76, 255});
    SDL_Color textPrimary = isLight ? SDL_Color{15, 23, 42, 255} : SDL_Color{248, 250, 252, 255};

    drawRoundedBox(r, rect, 10, pillBg);
    drawRoundedBorder(r, rect, 10, 1, pillBrd);

    int dotX = rect.x + 14;
    int dotY = rect.y + rect.h / 2;

    SDL_Color dotCol = SDL_Color{239, 68, 68, 255};
    if (wsState == WsState::Connecting) {
        dotCol = SDL_Color{234, 179, 8, 255};
    } else if (wsState == WsState::Connected) {
        if (pingMs <= 0 || pingMs < 60) {
            dotCol = SDL_Color{34, 197, 94, 255};
        } else if (pingMs <= 130) {
            dotCol = SDL_Color{245, 158, 11, 255};
        } else {
            dotCol = SDL_Color{239, 68, 68, 255};
        }
    }
    drawFilledCircleAA(r, dotX, dotY, 6, {dotCol.r, dotCol.g, dotCol.b, 55});
    drawFilledCircleAA(r, dotX, dotY, 4, dotCol);

    std::string connLabel;
    if (language == 1) {
        connLabel = (wsState == WsState::Connected) ? "Connected"
                  : (wsState == WsState::Connecting ? "Connecting" : "Offline");
    } else {
        connLabel = (wsState == WsState::Connected) ? "Bağlı"
                  : (wsState == WsState::Connecting ? "Bağlanıyor" : "Bağlantı Yok");
    }
    if (wsState == WsState::Connected && pingMs > 0) {
        connLabel += " " + std::to_string(pingMs) + " ms";
    }

    if (fontSmall) {
        int texW = 0, texH = 0;
        SDL_Texture* tex = getCachedTextTexture(r, fontSmall, connLabel, texW, texH);
        const int availableWidth = rect.w - 33;
        if (texW > availableWidth && fontTiny) {
            tex = getCachedTextTexture(r, fontTiny, connLabel, texW, texH);
        }
        if (tex && texW > 0 && availableWidth > 0) {
            SDL_SetTextureColorMod(tex, textPrimary.r, textPrimary.g, textPrimary.b);
            SDL_SetTextureAlphaMod(tex, textPrimary.a);
            SDL_Rect dst = { rect.x + 25, rect.y + (rect.h - texH) / 2, texW, texH };
            const SDL_Rect src{0, 0, std::min(texW, availableWidth), texH};
            dst.w = src.w;
            SDL_RenderCopy(r, tex, &src, &dst);
        }
    }
}

void UIRenderer::drawBatteryPill(SDL_Renderer* r, TTF_Font* fontSmall, const SDL_Rect& rect, int batteryPct, bool isCharging, int lowThreshold, bool isLight, bool isOled, int language) {
    if (!r || rect.w <= 0 || rect.h <= 0) return;

    bool isLowBat = (batteryPct <= lowThreshold && !isCharging && lowThreshold > 0);
    SDL_Color pillBg, pillBrd, textPrimary;

    if (isCharging) {
        pillBg = SDL_Color{14, 60, 36, 240};
        pillBrd = SDL_Color{34, 197, 94, 255};
        textPrimary = SDL_Color{74, 222, 128, 255};
    } else if (isLowBat) {
        pillBg = SDL_Color{127, 29, 29, 255};
        pillBrd = SDL_Color{239, 68, 68, 255};
        textPrimary = SDL_Color{255, 255, 255, 255};
    } else {
        pillBg = isOled ? SDL_Color{20, 24, 34, 255}
                        : (isLight ? SDL_Color{241, 245, 249, 255} : SDL_Color{24, 30, 44, 255});
        pillBrd = isOled ? SDL_Color{38, 46, 64, 255}
                         : (isLight ? SDL_Color{203, 213, 225, 255} : SDL_Color{44, 54, 76, 255});
        textPrimary = isLight ? SDL_Color{15, 23, 42, 255} : SDL_Color{248, 250, 252, 255};
    }

    drawRoundedBox(r, rect, 10, pillBg);
    drawRoundedBorder(r, rect, 10, 1, pillBrd);

    if (isLowBat) {
        std::string warnStr = (language == 1 ? "! LOW %" : "! PİL %") + std::to_string(std::clamp(batteryPct, 0, 100));
        if (fontSmall) {
            int texW = 0, texH = 0;
            SDL_Texture* tex = getCachedTextTexture(r, fontSmall, warnStr, texW, texH);
            if (tex && texW > 0) {
                SDL_SetTextureColorMod(tex, textPrimary.r, textPrimary.g, textPrimary.b);
                SDL_SetTextureAlphaMod(tex, textPrimary.a);
                SDL_Rect dst = { rect.x + (rect.w - texW) / 2, rect.y + (rect.h - texH) / 2, texW, texH };
                SDL_RenderCopy(r, tex, nullptr, &dst);
            }
        }
        return;
    }

    const int percentage = std::clamp(batteryPct, 0, 100);
    std::string label = language == 1 ? std::to_string(percentage) + "%" : "%" + std::to_string(percentage);
    if (isCharging) label += language == 1 ? "  Charging" : "  Şarj";
    int texW = 0, texH = 0;
    auto* tex = fontSmall ? getCachedTextTexture(r, fontSmall, label, texW, texH) : nullptr;
    constexpr int iconWidth = 25;
    constexpr int textGap = 8;
    const int contentWidth = iconWidth + (tex ? textGap + texW : 0);
    const int batX = rect.x + (rect.w - contentWidth) / 2;
    int batY = rect.y + (rect.h - 14) / 2;
    SDL_Rect batOutline = { batX, batY, 22, 14 };
    drawRoundedBorder(r, batOutline, 3, 1, textPrimary);
    boxRGBA(r, batOutline.x + batOutline.w, batOutline.y + 4, batOutline.x + batOutline.w + 2, batOutline.y + 9,
            textPrimary.r, textPrimary.g, textPrimary.b, 255);

    const int fillW = (18 * percentage) / 100;
    SDL_Color batFill = isCharging ? SDL_Color{34, 197, 94, 255}
                      : (batteryPct < 20 ? SDL_Color{239, 68, 68, 255} : textPrimary);
    if (fillW > 0) {
        boxRGBA(r, batOutline.x + 2, batOutline.y + 2, batOutline.x + 1 + fillW, batOutline.y + batOutline.h - 3,
                batFill.r, batFill.g, batFill.b, 255);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    if (tex) {
        SDL_SetTextureColorMod(tex, textPrimary.r, textPrimary.g, textPrimary.b);
        SDL_SetTextureAlphaMod(tex, textPrimary.a);
        const SDL_Rect dst{batX + iconWidth + textGap, rect.y + (rect.h - texH) / 2, texW, texH};
        SDL_RenderCopy(r, tex, nullptr, &dst);
    }
}

void UIRenderer::renderUpdateModal(const std::string& newVersion) {
    if (!renderer_) return;

    boxRGBA(renderer_, 0, 0, SCREEN_W - 1, SCREEN_H - 1, 0, 0, 0, 195);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    int mw = 540;
    int mh = 330;
    int mx = (SCREEN_W - mw) / 2;
    int my = (SCREEN_H - mh) / 2;
    SDL_Rect modalRect = { mx, my, mw, mh };

    drawRoundedBox(renderer_, modalRect, 18, {20, 25, 36, 255});
    drawRoundedBorder(renderer_, modalRect, 18, 2, {59, 130, 246, 255});

    SDL_Rect badgeRect = { mx + (mw - 240) / 2, my + 24, 240, 32 };
    drawRoundedBox(renderer_, badgeRect, 16, {34, 197, 94, 230});
    drawText(fontTiny_, "YENİ GÜNCELLEME MEVCUT", badgeRect.x + badgeRect.w / 2, badgeRect.y + badgeRect.h / 2, {255, 255, 255, 255}, true);

    drawText(fontLarge_, "SwitchDeck Güncellemesi", mx + mw / 2, my + 82, {255, 255, 255, 255}, true);

    std::string verStr = "Yeni Sürüm: " + (newVersion.empty() ? "Mevcut" : newVersion);
    drawText(fontMedium_, verStr, mx + mw / 2, my + 130, {147, 197, 253, 255}, true);

    drawText(fontSmall_, "GitHub üzerinden yeni bir SwitchDeck sürümü yayınlandı.", mx + mw / 2, my + 176, {203, 213, 225, 255}, true);
    drawText(fontSmall_, "Ayarlar menüsünden veya GitHub Releases sayfasından indirebilirsiniz.", mx + mw / 2, my + 206, {148, 163, 184, 255}, true);

    rectUpdateModalDismiss_ = { mx + (mw - 240) / 2, my + 248, 240, 48 };
    drawRoundedBox(renderer_, rectUpdateModalDismiss_, 12, {37, 99, 235, 255});
    drawRoundedBorder(renderer_, rectUpdateModalDismiss_, 12, 1, {96, 165, 250, 255});
    drawText(fontNormal_, "Anladım (Kapat)", rectUpdateModalDismiss_.x + rectUpdateModalDismiss_.w / 2, rectUpdateModalDismiss_.y + rectUpdateModalDismiss_.h / 2, {255, 255, 255, 255}, true);
}

void UIRenderer::renderEmptyCell(const SDL_Rect& rect, int radius, bool isFocused, int theme) {
    if (theme == 1) { // Light theme
        drawRoundedBox(renderer_, rect, radius, {255, 255, 255, 130});
    } else { // Macro Deck 3 Dark (--color-bg-secondary: #1a1a1a; opacity: 0.35) & OLED
        drawRoundedBox(renderer_, rect, radius, {26, 26, 26, 89});
    }
    if (isFocused) {
        // Macro Deck 3 two-tone focus ring: inset 2px #fff, inset 5px var(--color-accent) (#2196f3)
        drawRoundedBorder(renderer_, rect, radius, 2, {255, 255, 255, 255});
        SDL_Rect outerFocus = { rect.x - 3, rect.y - 3, rect.w + 6, rect.h + 6 };
        drawRoundedBorder(renderer_, outerFocus, radius + 3, 3, {33, 150, 243, 255});
    }
}

void UIRenderer::renderPageControls(const DeckState& state, SDL_Color background, SDL_Color border,
                                    SDL_Color text, SDL_Color arrow) {
    int fCount = state.getFolderCount();
    int curIdx = state.getCurrentFolderIndex();
    std::string folderStr = state.getCurrentFolderName();
    if (fCount > 1) {
        folderStr += "  (" + std::to_string(curIdx + 1) + "/" + std::to_string(fCount) + ")";
    }

    if (fCount > 1) drawRoundedBox(renderer_, rectBarFolderPrev_, 10, background);
    if (fCount > 1) drawRoundedBorder(renderer_, rectBarFolderPrev_, 10, 1, border);
    if (fCount > 1) drawText(fontSmall_, "<", rectBarFolderPrev_.x + rectBarFolderPrev_.w / 2, rectBarFolderPrev_.y + rectBarFolderPrev_.h / 2, arrow, true);

    drawRoundedBox(renderer_, rectBarFolderPill_, 10, background);
    drawRoundedBorder(renderer_, rectBarFolderPill_, 10, 1, border);
    drawText(fontSmall_, folderStr, rectBarFolderPill_.x + rectBarFolderPill_.w / 2, rectBarFolderPill_.y + rectBarFolderPill_.h / 2, text, true, rectBarFolderPill_.w - 20);

    if (fCount > 1) drawRoundedBox(renderer_, rectBarFolderNext_, 10, background);
    if (fCount > 1) drawRoundedBorder(renderer_, rectBarFolderNext_, 10, 1, border);
    if (fCount > 1) drawText(fontSmall_, ">", rectBarFolderNext_.x + rectBarFolderNext_.w / 2, rectBarFolderNext_.y + rectBarFolderNext_.h / 2, arrow, true);

}

void UIRenderer::renderFloatingControls(const DeckState& state, int language) {
    const SDL_Color background{18, 22, 32, 235}, border{255, 255, 255, 40}, text{240, 244, 248, 255};
    renderPageControls(state, background, border, text, text);

    if (state.canGoBackOrParent()) {
        drawRoundedBox(renderer_, rectFloatingBack_, 10, background);
        drawRoundedBorder(renderer_, rectFloatingBack_, 10, 1, border);
        drawText(fontSmall_, language == 1 ? "< Back" : "< Geri", rectFloatingBack_.x + rectFloatingBack_.w / 2,
                 rectFloatingBack_.y + rectFloatingBack_.h / 2, text, true);
    }

    drawRoundedBox(renderer_, rectFloatingSettings_, 10, {37, 99, 235, 235});
    drawRoundedBorder(renderer_, rectFloatingSettings_, 10, 1, {96, 165, 250, 255});
    drawText(fontSmall_, language == 1 ? "Settings" : "Ayarlar", rectFloatingSettings_.x + rectFloatingSettings_.w / 2,
             rectFloatingSettings_.y + rectFloatingSettings_.h / 2, text, true);
}


// ============================================================================
// Widget Tile Dispatcher & Specialized Renderers
// ============================================================================

void UIRenderer::renderWidgetTile(const DeckButton& btn, const SDL_Rect& rect, int radius, bool isFocused, const DeckState& state, const AppConfig& config, int batteryPct, bool isCharging) {
    if (renderWidgetTree(btn, rect, state, config, radius, isFocused)) return;

    SDL_Rect drawRect = rect;
    int drawRadius = radius;

    // Instant tactile press depth (+1px Y nudge when held, no slow resize jitter)
    if (btn.visualPressFraction > 0.35f) {
        drawRect.y += 1;
    }

    // Macro Deck 3 box shadow: --shadow-sm: 0 4px 6px -1px rgba(0, 0, 0, 0.3)
    SDL_Rect shadowRect = { drawRect.x, drawRect.y + 4, drawRect.w, drawRect.h };
    drawRoundedBox(renderer_, shadowRect, drawRadius, {0, 0, 0, 77});

    // Clip all widget drawing strictly to drawRect so zoomed/offset images never escape the tile
    SDL_RenderSetClipRect(renderer_, &drawRect);

    if (btn.widgetType == "MusicPlayer") {
        renderMusicPlayerWidget(btn, drawRect, drawRadius, config);
    } else if (btn.widgetType == "Slider") {
        renderSliderWidget(btn, drawRect, drawRadius, config);
    } else if (btn.widgetType == "Clock") {
        renderClockWidget(btn, drawRect, drawRadius, config);
    } else if (btn.widgetType == "Weather") {
        renderWeatherWidget(btn, drawRect, drawRadius, state, config);
    } else if (btn.widgetType == "Gauges") {
        renderGaugesWidget(btn, drawRect, drawRadius, state, config);
    } else if (btn.widgetType == "Countdown") {
        renderCountdownWidget(btn, drawRect, drawRadius, config);
    } else if (btn.widgetType == "Battery") {
        renderBatteryWidget(btn, drawRect, drawRadius, batteryPct, isCharging, state, config);
    } else if (btn.widgetType == "Stopwatch") {
        renderStopwatchWidget(btn, drawRect, drawRadius, config);
    } else if (btn.widgetType == "Calendar") {
        renderCalendarWidget(btn, drawRect, drawRadius, config);
    } else if (btn.widgetType == "HistoryGraph") {
        renderHistoryGraphWidget(btn, drawRect, drawRadius, config);
    } else {
        renderActionButtonWidget(btn, drawRect, drawRadius, config);
    }

    // Hardware-cached rounded corner mask back to currentCanvasBg_
    maskRoundedCorners(renderer_, drawRect, drawRadius, currentCanvasBg_);

    SDL_RenderSetClipRect(renderer_, nullptr);

    bool borderOff = btn.borderExplicitlyOff;
    if (!btn.borderStyle.empty() && !borderOff) {
        renderWidgetBorder(btn, drawRect, drawRadius, state, config);
    } else if (btn.hasBorder && !borderOff) {
        drawRoundedBorder(renderer_, drawRect, drawRadius, 2, {btn.borderR, btn.borderG, btn.borderB, 255});
    } else if (config.theme != 1 && (btn.widgetType != "ActionButton" || btn.iconTexture == nullptr) && !borderOff) {
        // Subtle 1px ambient rim on non-artwork tiles for crisp modern depth
        drawRoundedBorder(renderer_, drawRect, drawRadius, 1, {255, 255, 255, 16});
    }

    // Macro Deck 3 press overlay: background: rgba(255, 255, 255, 0.2)
    if (btn.visualPressFraction > 0.001f) {
        Uint8 overlayAlpha = static_cast<Uint8>(std::round(51.0f * btn.visualPressFraction));
        if (overlayAlpha > 0) {
            drawRoundedBox(renderer_, drawRect, drawRadius, {255, 255, 255, overlayAlpha});
        }
    }

    // Macro Deck 3 two-tone focus ring: inset 2px #fff, inset 5px var(--color-accent) (#2196f3)
    if (isFocused) {
        drawRoundedBorder(renderer_, drawRect, drawRadius, 2, {255, 255, 255, 255});
        SDL_Rect outerFocus = { drawRect.x - 3, drawRect.y - 3, drawRect.w + 6, drawRect.h + 6 };
        drawRoundedBorder(renderer_, outerFocus, drawRadius + 3, 3, {33, 150, 243, 255});
    }
}

void UIRenderer::drawProviderIcon(const std::string& providerName, int cx, int cy, int radius) {
    bool isSpotify = (providerName.find("Spotify") != std::string::npos ||
                      providerName.find("spotify") != std::string::npos ||
                      providerName.empty());
    if (isSpotify) {
        drawFilledCircleAA(renderer_, cx, cy, radius, {29, 185, 84, 255});
        int w1 = std::max(3, radius - 3);
        int w2 = std::max(2, radius - 4);
        int w3 = std::max(2, radius - 5);
        thickLineRGBA(renderer_, cx - w1, cy - 3, cx + w1, cy - 2, 2, 18, 18, 18, 240);
        thickLineRGBA(renderer_, cx - w2, cy,     cx + w2, cy + 1, 2, 18, 18, 18, 240);
        thickLineRGBA(renderer_, cx - w3, cy + 3, cx + w3, cy + 4, 2, 18, 18, 18, 240);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    } else {
        drawFilledCircleAA(renderer_, cx, cy, radius, {33, 150, 243, 255});
        drawVectorIcon("music", cx, cy, radius, {255, 255, 255, 255});
    }
}

static SDL_Color getVisibleTileBg(const DeckButton& btn, int theme) {
    if (btn.hasBgColor) {
        return {btn.bgR, btn.bgG, btn.bgB, 255};
    }
    if (theme == 1) { // Light theme
        return {255, 255, 255, 255};
    } else if (theme == 2) { // OLED Black
        return {10, 10, 12, 255};
    }
    // Macro Deck 3 default tile background: --color-bg-secondary #1a1a1a
    return {26, 26, 26, 255};
}

void UIRenderer::drawSmoothArcAA(SDL_Renderer* r, int cx, int cy, int radius, int thickness, float startDeg, float endDeg, SDL_Color col) {
    if (!r || radius <= 2 || thickness <= 0 || endDeg <= startDeg || col.a == 0) return;
    int dotR = std::max(1, thickness / 2);
    float step = std::clamp(90.0f / static_cast<float>(std::max(12, radius)), 1.5f, 4.0f);
    for (float deg = startDeg; deg <= endDeg; deg += step) {
        float rad = deg * (3.1415926535f / 180.0f);
        int px = cx + static_cast<int>(std::round(std::cos(rad) * radius));
        int py = cy + static_cast<int>(std::round(std::sin(rad) * radius));
        filledCircleRGBA(r, static_cast<Sint16>(px), static_cast<Sint16>(py), static_cast<Sint16>(dotR), col.r, col.g, col.b, col.a);
    }
    float endRad = endDeg * (3.1415926535f / 180.0f);
    int ex = cx + static_cast<int>(std::round(std::cos(endRad) * radius));
    int ey = cy + static_cast<int>(std::round(std::sin(endRad) * radius));
    filledCircleRGBA(r, static_cast<Sint16>(ex), static_cast<Sint16>(ey), static_cast<Sint16>(dotR), col.r, col.g, col.b, col.a);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

static void drawWeatherSymbol(SDL_Renderer* r, const std::string& hintStr, int cx, int cy, int size) {
    if (!r || size <= 4) return;
    std::string h = hintStr;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    bool isNight   = (h.find("night") != std::string::npos);
    bool isThunder = (h.find("thunder") != std::string::npos || h.find("storm") != std::string::npos || h.find("fırtına") != std::string::npos);
    bool isSnow    = (h.find("snow") != std::string::npos || h.find("kar") != std::string::npos || h.find("sleet") != std::string::npos);
    bool isRain    = (h.find("rain") != std::string::npos || h.find("drizzle") != std::string::npos || h.find("shower") != std::string::npos || h.find("yağmur") != std::string::npos || h.find("sağanak") != std::string::npos);
    bool isFog     = (h.find("fog") != std::string::npos || h.find("mist") != std::string::npos || h.find("sis") != std::string::npos);
    bool isPartly  = (h.find("cloudy-1") != std::string::npos || h.find("cloudy-2") != std::string::npos || h.find("partly") != std::string::npos || h.find("parçalı") != std::string::npos || h.find("az bulut") != std::string::npos);
    bool isCloudy  = isPartly || isRain || isSnow || isThunder || (h.find("cloud") != std::string::npos || h.find("overcast") != std::string::npos || h.find("bulut") != std::string::npos || h.find("kapalı") != std::string::npos);

    int rad = std::max(4, size / 2);

    auto drawCloud = [&](int ccx, int ccy, int cScale, SDL_Color col) {
        int rMain = std::max(3, (cScale * 36) / 100);
        int rLeft = std::max(2, (cScale * 26) / 100);
        int rRight = std::max(2, (cScale * 24) / 100);
        UIRenderer::drawFilledCircleAA(r, ccx - rLeft + 1, ccy + 2, rLeft, col);
        UIRenderer::drawFilledCircleAA(r, ccx + rRight - 1, ccy + 3, rRight, col);
        UIRenderer::drawFilledCircleAA(r, ccx, ccy - 2, rMain, col);
        SDL_Rect base = { ccx - rLeft, ccy + 1, rLeft + rRight, rLeft + 2 };
        UIRenderer::drawRoundedBox(r, base, 3, col);
    };

    if (!isCloudy && !isFog) {
        if (isNight) {
            UIRenderer::drawFilledCircleAA(r, cx, cy, (rad * 75) / 100, {250, 204, 21, 50});
            UIRenderer::drawFilledCircleAA(r, cx, cy, (rad * 58) / 100, {253, 224, 71, 255});
            UIRenderer::drawFilledCircleAA(r, cx + std::max(2, rad / 4), cy - std::max(2, rad / 5), (rad * 46) / 100, {24, 28, 38, 255});
        } else {
            int sunR = std::max(3, (rad * 56) / 100);
            UIRenderer::drawFilledCircleAA(r, cx, cy, (rad * 84) / 100, {250, 204, 21, 55});
            for (int i = 0; i < 8; ++i) {
                float a = i * (3.14159265f / 4.0f);
                int x1 = cx + static_cast<int>(std::round(std::cos(a) * (sunR + 2)));
                int y1 = cy + static_cast<int>(std::round(std::sin(a) * (sunR + 2)));
                int x2 = cx + static_cast<int>(std::round(std::cos(a) * (rad - 1)));
                int y2 = cy + static_cast<int>(std::round(std::sin(a) * (rad - 1)));
                thickLineRGBA(r, x1, y1, x2, y2, std::max(1, size / 14), 251, 191, 36, 235);
            }
            UIRenderer::drawFilledCircleAA(r, cx, cy, sunR, {250, 204, 21, 255});
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        return;
    }

    if (isFog) {
        int w = std::max(8, (size * 75) / 100);
        int hBar = std::max(2, size / 10);
        for (int i = -1; i <= 1; ++i) {
            SDL_Rect bar = { cx - w / 2 + (i * 2), cy + i * (hBar + 3) - hBar / 2, w - std::abs(i) * 3, hBar };
            UIRenderer::drawRoundedBox(r, bar, hBar / 2, {148, 163, 184, 230});
        }
        return;
    }

    if (isPartly) {
        int sunR = std::max(3, (rad * 42) / 100);
        int sx = cx - std::max(3, rad / 3);
        int sy = cy - std::max(3, rad / 3);
        UIRenderer::drawFilledCircleAA(r, sx, sy, sunR + 2, {250, 204, 21, 60});
        UIRenderer::drawFilledCircleAA(r, sx, sy, sunR, isNight ? SDL_Color{253, 224, 71, 235} : SDL_Color{250, 204, 21, 255});
    }

    int cloudY = (isRain || isSnow || isThunder) ? (cy - std::max(2, rad / 4)) : (cy + 1);
    SDL_Color cloudCol = isThunder ? SDL_Color{148, 163, 184, 255} : SDL_Color{241, 245, 249, 255};
    drawCloud(cx + 2, cloudY + 1, size, {100, 116, 139, 120});
    drawCloud(cx, cloudY, size, cloudCol);

    if (isThunder) {
        int bx = cx;
        int by = cloudY + std::max(3, rad / 3);
        filledTrigonRGBA(r, bx + 2, by - 2, bx - 4, by + 4, bx + 1, by + 4, 250, 204, 21, 255);
        filledTrigonRGBA(r, bx - 1, by + 3, bx + 4, by + 3, bx - 2, by + 9, 250, 204, 21, 255);
    } else if (isRain) {
        int dropY = cloudY + std::max(4, rad / 2);
        for (int i = -1; i <= 1; ++i) {
            int dx = cx + i * std::max(4, rad / 2);
            thickLineRGBA(r, dx, dropY, dx - 2, dropY + std::max(3, rad / 3), std::max(1, size / 16), 56, 189, 248, 245);
        }
    } else if (isSnow) {
        int snowY = cloudY + std::max(5, rad / 2 + 1);
        for (int i = -1; i <= 1; ++i) {
            int sx = cx + i * std::max(4, rad / 2);
            UIRenderer::drawFilledCircleAA(r, sx, snowY, std::max(1, size / 14), {224, 242, 254, 255});
        }
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

void UIRenderer::renderMusicPlayerWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    Uint64 nowTicks = SDL_GetTicks64();
    float progFrac = btn.getCurrentProgressFraction(nowTicks);
    SDL_Color accentCol = btn.hasAccentColor
        ? SDL_Color{btn.accentR, btn.accentG, btn.accentB, 255}
        : SDL_Color{30, 215, 96, 255};

    std::string cs = btn.musicCoverStyle;
    for (char& c : cs) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool isFullCover = (cs == "full" || cs == "cover" || cs == "artwork" || cs == "background" || cs == "fullcover");
    std::string titleStr = btn.musicTitle.empty() ? (btn.labelText.empty() ? "Nothing playing" : btn.labelText) : btn.musicTitle;
    std::string artistStr = btn.musicArtist;

    const bool hasCover = btn.iconTexture && btn.iconWidth > 0 && btn.iconHeight > 0 &&
                          (btn.musicArtworkId.empty() || btn.loadedResourceId == btn.resourceId);
    if (isFullCover) {
        if (hasCover) {
            float scaleX = static_cast<float>(drawRect.w) / btn.iconWidth;
            float scaleY = static_cast<float>(drawRect.h) / btn.iconHeight;
            float scale = std::max(scaleX, scaleY);
            int srcW = static_cast<int>(drawRect.w / scale);
            int srcH = static_cast<int>(drawRect.h / scale);
            int srcX = (btn.iconWidth - srcW) / 2;
            int srcY = (btn.iconHeight - srcH) / 2;
            SDL_Rect srcRect = { std::max(0, srcX), std::max(0, srcY), std::min(btn.iconWidth, srcW), std::min(btn.iconHeight, srcH) };
            SDL_RenderCopy(renderer_, btn.iconTexture, &srcRect, &drawRect);
        } else {
            drawVectorIcon("music", drawRect.x + drawRect.w / 2, drawRect.y + drawRect.h / 2 - 16, 44, {70, 78, 92, 255});
        }

        // Smooth dark gradient scrim on lower portion so text & progress bar are crystal clear
        int scrimStartY = drawRect.y + static_cast<int>(drawRect.h * 0.30f);
        int scrimH = (drawRect.y + drawRect.h) - scrimStartY;
        int numSteps = 24;
        int stepH = std::max(1, scrimH / numSteps);
        for (int i = 0; i < numSteps; ++i) {
            int y1 = scrimStartY + i * stepH;
            int y2 = (i == numSteps - 1) ? (drawRect.y + drawRect.h) : (y1 + stepH);
            float t = static_cast<float>(i + 1) / static_cast<float>(numSteps);
            Uint8 alpha = static_cast<Uint8>(std::clamp(t * t * 240.0f, 0.0f, 240.0f));
            boxRGBA(renderer_, drawRect.x, y1, drawRect.x + drawRect.w - 1, y2, 8, 12, 18, alpha);
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

        int tlPad = 20;
        int barX = drawRect.x + tlPad;
        int barW = std::max(24, drawRect.w - 2 * tlPad);
        int barH = 5;
        int barY = drawRect.y + drawRect.h - (btn.musicShowTimeline ? 32 : 16);
        TTF_Font* titleFont = (drawRect.w >= 240 && drawRect.h >= 150) ? fontMedium_ : fontNormal_;
        TTF_Font* subFont = fontSmall_;

        int artistCy = btn.musicShowTimeline ? (barY - 18) : (drawRect.y + drawRect.h - 20);
        int titleCy  = artistStr.empty() ? (artistCy - 4) : (artistCy - 22);

        // Left-align at the start of the progress bar line (barX)
        drawText(titleFont, titleStr, barX, titleCy, {255, 255, 255, 255}, false, barW, false, true);
        if (!artistStr.empty()) {
            drawText(subFont, artistStr, barX, artistCy, {203, 213, 225, 245}, false, barW, false, true);
        }

        if (btn.musicShowTimeline) {
            SDL_Rect trackRect = { barX, barY, barW, barH };
            drawRoundedBox(renderer_, trackRect, 3, {255, 255, 255, 60});

            int fillW = static_cast<int>(std::round(barW * progFrac));
            if (fillW > 0) {
                SDL_Rect fillRect = { barX, barY, std::max(barH, fillW), barH };
                drawRoundedBox(renderer_, fillRect, 3, accentCol);
            }

            std::string elapsedStr = DeckButton::formatDurationMs(btn.getCurrentProgressMs(nowTicks));
            std::string durationStr = DeckButton::formatDurationMs(btn.progressDurationMs);
            int timeY = barY + barH + 4;
            drawText(fontTiny_, elapsedStr, barX, timeY, {200, 210, 225, 240}, false, 60, false, true);
            drawText(fontTiny_, durationStr, barX + barW, timeY, {200, 210, 225, 240}, false, 60, true, true);
        }
    } else if (drawRect.h >= 150 && drawRect.w >= 140) {
        // Multi-row small cover layout (e.g. 2x2, 3x2, 2x3) - large centered artwork + centered info + timeline
        int tlPad = 20;
        int barX = drawRect.x + tlPad;
        int barW = std::max(24, drawRect.w - 2 * tlPad);
        int barH = 5;
        int barY = drawRect.y + drawRect.h - 32;

        int upperH = btn.musicShowTimeline ? (barY - drawRect.y - 10) : (drawRect.h - 18);
        TTF_Font* titleFont = (drawRect.w >= 260) ? fontMedium_ : fontNormal_;
        TTF_Font* subFont   = fontSmall_;

        int titleSkip = btn.musicShowTitle ? TTF_FontLineSkip(titleFont) : 0;
        int subSkip = (!btn.musicShowArtist || artistStr.empty()) ? 0 : (TTF_FontLineSkip(subFont) + 2);
        int infoBlockH = (titleSkip > 0 || subSkip > 0) ? (10 + titleSkip + subSkip) : 0;

        int maxCoverW = std::max(64, drawRect.w - 36);
        int maxCoverH = std::max(64, upperH - infoBlockH - 12);
        int coverSize = std::min(maxCoverW, maxCoverH);
        int targetCover = static_cast<int>(std::round(std::min(drawRect.w, upperH) * 0.72f));
        coverSize = std::clamp(targetCover, 64, std::min(210, coverSize));
        int coverRad = 12;

        int totalContentH = coverSize + infoBlockH;
        int startY = drawRect.y + std::max(8, (upperH - totalContentH) / 2);

        int coverX = drawRect.x + (drawRect.w - coverSize) / 2;
        int coverY = startY;
        SDL_Rect coverRect = { coverX, coverY, coverSize, coverSize };

        if (hasCover) {
            SDL_Rect covShadow = { coverRect.x, coverRect.y + 3, coverRect.w, coverRect.h };
            drawRoundedBox(renderer_, covShadow, coverRad, {0, 0, 0, 90});

            // Center-crop to 1:1 aspect ratio before rendering so album art is never distorted
            int side = std::min(btn.iconWidth, btn.iconHeight);
            int srcX = (btn.iconWidth - side) / 2;
            int srcY = (btn.iconHeight - side) / 2;
            SDL_Rect srcRect = { srcX, srcY, side, side };
            SDL_RenderCopy(renderer_, btn.iconTexture, &srcRect, &coverRect);
            if (coverRad > 1) {
                maskRoundedCorners(renderer_, coverRect, coverRad, bgCol);
            }
        } else {
            drawRoundedBox(renderer_, coverRect, coverRad, {255, 255, 255, 16});
            drawRoundedBorder(renderer_, coverRect, coverRad, 1, {255, 255, 255, 24});
            int discCx = coverRect.x + coverRect.w / 2;
            int discCy = coverRect.y + coverRect.h / 2;
            int discR = std::max(10, coverSize / 3);
            drawFilledCircleAA(renderer_, discCx, discCy, discR, {38, 42, 50, 220}, {150, 155, 165, 140}, 2);
            drawFilledCircleAA(renderer_, discCx, discCy, std::max(4, discR / 3), accentCol);
        }

        // Centered Title & Artist directly below cover
        int titleCy = coverY + coverSize + 8 + titleSkip / 2;
        drawText(titleFont, titleStr, drawRect.x + drawRect.w / 2, titleCy, {255, 255, 255, 255}, true, drawRect.w - 32, false, false, true);

        if (!artistStr.empty()) {
            int artistCy = titleCy + titleSkip / 2 + 2 + TTF_FontLineSkip(subFont) / 2;
            drawText(subFont, artistStr, drawRect.x + drawRect.w / 2, artistCy, {160, 172, 188, 245}, true, drawRect.w - 32, false, false, true);
        }

        // Full-width bottom progress bar + elapsed/duration timestamps underneath (1:1 Macro Deck 3)
        if (btn.musicShowTimeline) {
            SDL_Rect trackRect = { barX, barY, barW, barH };
            drawRoundedBox(renderer_, trackRect, 3, {44, 56, 68, 255});

            int fillW = static_cast<int>(std::round(barW * progFrac));
            if (fillW > 0) {
                SDL_Rect fillRect = { barX, barY, std::max(barH, fillW), barH };
                drawRoundedBox(renderer_, fillRect, 3, accentCol);
            }

            std::string elapsedStr = DeckButton::formatDurationMs(btn.getCurrentProgressMs(nowTicks));
            std::string durationStr = DeckButton::formatDurationMs(btn.progressDurationMs);
            int timeY = barY + barH + 4;
            drawText(fontTiny_, elapsedStr, barX, timeY, {140, 152, 168, 235}, false, 56);
            drawText(fontTiny_, durationStr, barX + barW, timeY, {140, 152, 168, 235}, false, 56, true);
        }
    } else {
        // Compact horizontal layout for 1-row tiles (e.g. 2x1 or 1x1)
        int padX = 14;
        int padY = 10;
        int innerX = drawRect.x + padX;
        int innerW = std::max(20, drawRect.w - 2 * padX);
        int curY = drawRect.y + padY;
        int bottomLimitY = drawRect.y + drawRect.h - padY;

        bool showTimeline = btn.musicShowTimeline && (btn.hasProgressBar || btn.progressDurationMs > 0.0) && (bottomLimitY - curY >= 48);
        int timelineH = showTimeline ? 22 : 0;
        int bodyH = std::max(20, (bottomLimitY - (showTimeline ? (timelineH + 4) : 0)) - curY);

        int coverSize = std::clamp(bodyH, 32, 110);
        int coverY = curY + (bodyH - coverSize) / 2;
        SDL_Rect coverRect = { innerX, coverY, coverSize, coverSize };
        int coverRad = 6;

        if (hasCover) {
            SDL_Rect covShadow = { coverRect.x, coverRect.y + 2, coverRect.w, coverRect.h };
            drawRoundedBox(renderer_, covShadow, coverRad, {0, 0, 0, 90});
            SDL_RenderCopy(renderer_, btn.iconTexture, nullptr, &coverRect);
            maskRoundedCorners(renderer_, coverRect, coverRad, bgCol);
        } else {
            drawRoundedBox(renderer_, coverRect, coverRad, {255, 255, 255, 20});
        }
        int infoX = coverRect.x + coverRect.w + 12;
        int infoW = std::max(20, (innerX + innerW) - infoX);

        TTF_Font* titleFont = fontNormal_;
        TTF_Font* subFont = fontSmall_;
        int textY = curY + std::max(0, (bodyH - (TTF_FontLineSkip(titleFont) + TTF_FontLineSkip(subFont))) / 2);

        drawText(titleFont, titleStr, infoX, textY, {255, 255, 255, 255}, false, infoW);
        textY += TTF_FontLineSkip(titleFont) + 2;
        if (!artistStr.empty()) {
            drawText(subFont, artistStr, infoX, textY, {160, 172, 188, 240}, false, infoW);
        }

        if (showTimeline) {
            int barY = bottomLimitY - timelineH;
            int barH = 5;
            SDL_Rect trackRect = { innerX, barY, innerW, barH };
            drawRoundedBox(renderer_, trackRect, 3, {44, 56, 68, 255});
            int fillW = static_cast<int>(std::round(innerW * progFrac));
            if (fillW > 0) {
                SDL_Rect fillRect = { innerX, barY, std::max(barH, fillW), barH };
                drawRoundedBox(renderer_, fillRect, 3, {186, 196, 208, 255});
            }
        }
    }

    // Top-Right Equalizer badge (matching Macro Deck 3 circular badge + 3 bars)
    if (btn.musicIsPlaying || btn.musicIsPaused) {
        int badgeCx = drawRect.x + drawRect.w - 26;
        int badgeCy = drawRect.y + 24;
        drawFilledCircleAA(renderer_, badgeCx, badgeCy, 13, {12, 16, 22, 210});
        int eqX = badgeCx - 6;
        int eqBottom = badgeCy + 6;
        if (btn.musicIsPlaying) {
            float t = static_cast<float>(nowTicks) / 1000.0f;
            int h1 = 5 + static_cast<int>(5.0f * (0.5f + 0.5f * std::sin(t * 7.0f)));
            int h2 = 7 + static_cast<int>(6.0f * (0.5f + 0.5f * std::sin(t * 9.5f + 1.2f)));
            int h3 = 4 + static_cast<int>(6.0f * (0.5f + 0.5f * std::sin(t * 6.2f + 2.5f)));
            boxRGBA(renderer_, eqX,     eqBottom - h1, eqX + 2,  eqBottom, 255, 255, 255, 255);
            boxRGBA(renderer_, eqX + 5, eqBottom - h2, eqX + 7,  eqBottom, 255, 255, 255, 255);
            boxRGBA(renderer_, eqX + 10, eqBottom - h3, eqX + 12, eqBottom, 255, 255, 255, 255);
        } else {
            boxRGBA(renderer_, eqX,     eqBottom - 5, eqX + 2,  eqBottom, 180, 190, 205, 220);
            boxRGBA(renderer_, eqX + 5, eqBottom - 8, eqX + 7,  eqBottom, 180, 190, 205, 220);
            boxRGBA(renderer_, eqX + 10, eqBottom - 4, eqX + 12, eqBottom, 180, 190, 205, 220);
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    }
}

void UIRenderer::renderSliderWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color accentCol = btn.hasAccentColor
        ? SDL_Color{btn.accentR, btn.accentG, btn.accentB, 255}
        : SDL_Color{30, 215, 96, 255}; // Spotify Green #1ed760

    float level = (btn.visualSliderLevel >= 0.0f) ? btn.visualSliderLevel : btn.sliderLevel;
    level = std::clamp(level, 0.0f, 1.0f);

    std::string valStr = btn.sliderValueText;
    if (valStr.empty()) {
        int pct = static_cast<int>(std::round(level * 100.0f));
        valStr = std::to_string(pct) + " %";
    }

    if (btn.sliderIsVertical) {
        // Macro Deck 3 vertical capsule slider (wide touch-friendly track + glowing fill + clean value label)
        int thickness = std::clamp(drawRect.w / 3, 24, 38);
        int topPad = (btn.sliderShowLabel && !btn.labelText.empty()) ? 34 : 16;
        int bottomPad = btn.sliderShowValue ? 36 : 16;
        int trackH = std::max(24, drawRect.h - topPad - bottomPad);
        int trackX = drawRect.x + (drawRect.w - thickness) / 2;
        int trackY = drawRect.y + topPad;

        if (btn.sliderShowLabel && !btn.labelText.empty()) {
            drawText(fontSmall_, btn.labelText, drawRect.x + drawRect.w / 2, drawRect.y + 16, {235, 240, 248, 240}, true, drawRect.w - 10);
        }

        SDL_Rect trackRect = { trackX, trackY, thickness, trackH };
        drawRoundedBox(renderer_, trackRect, thickness / 2, {36, 35, 42, 255});

        int fillH = static_cast<int>(std::round(trackH * level));
        if (fillH > 0) {
            fillH = std::min(trackH, std::max(thickness / 2, fillH));
            SDL_Rect fillRect = { trackX, trackY + trackH - fillH, thickness, fillH };
            drawRoundedBox(renderer_, fillRect, thickness / 2, accentCol);
        }

        int thumbRadius = thickness / 2 + 5;
        int thumbCx = trackX + thickness / 2;
        int thumbCy = trackY + trackH - static_cast<int>(std::round(trackH * level));
        thumbCy = std::clamp(thumbCy, trackY + thumbRadius, trackY + trackH - thumbRadius);

        drawFilledCircleAA(renderer_, thumbCx, thumbCy, thumbRadius + 2, bgCol);
        drawFilledCircleAA(renderer_, thumbCx, thumbCy, thumbRadius, {255, 255, 255, 255});

        if (btn.sliderShowValue) {
            int textY = drawRect.y + drawRect.h - 17;
            drawText(fontNormal_, valStr, drawRect.x + drawRect.w / 2, textY, {255, 255, 255, 255}, true, drawRect.w - 10);
        }
    } else {
        // Horizontal Slider (wide touch-friendly track + thumb)
        int thickness = std::clamp(drawRect.h / 4, 22, 32);
        int padX = 20;
        int availW = std::max(24, drawRect.w - 2 * padX);
        int trackX = drawRect.x + padX;
        int trackY = drawRect.y + (drawRect.h - thickness) / 2;
        if ((btn.sliderShowLabel && !btn.labelText.empty()) || btn.sliderShowValue) {
            trackY += 12;
        }

        if (btn.sliderShowLabel && !btn.labelText.empty()) {
            drawText(fontNormal_, btn.labelText, drawRect.x + padX, drawRect.y + 16, {245, 245, 245, 255}, false, availW - 70);
        }
        if (btn.sliderShowValue && !valStr.empty()) {
            drawText(fontNormal_, valStr, drawRect.x + drawRect.w - padX, drawRect.y + 16, {245, 245, 245, 255}, false, 72, true);
        }

        SDL_Rect trackRect = { trackX, trackY, availW, thickness };
        drawRoundedBox(renderer_, trackRect, thickness / 2, {36, 35, 42, 255});

        int fillW = static_cast<int>(std::round(availW * level));
        if (fillW > 0) {
            fillW = std::min(availW, std::max(thickness / 2, fillW));
            SDL_Rect fillRect = { trackX, trackY, fillW, thickness };
            drawRoundedBox(renderer_, fillRect, thickness / 2, accentCol);
        }

        int thumbRadius = thickness / 2 + 5;
        int thumbCx = trackX + static_cast<int>(std::round(availW * level));
        thumbCx = std::clamp(thumbCx, trackX + thumbRadius, trackX + availW - thumbRadius);
        int thumbCy = trackY + thickness / 2;

        drawFilledCircleAA(renderer_, thumbCx, thumbCy, thumbRadius + 2, bgCol);
        drawFilledCircleAA(renderer_, thumbCx, thumbCy, thumbRadius, {255, 255, 255, 255});
    }
}

void UIRenderer::renderClockWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color txtCol = btn.hasTextColor
        ? SDL_Color{btn.textR, btn.textG, btn.textB, 255}
        : (config.theme == 1 ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255});
    SDL_Color subCol = {txtCol.r, txtCol.g, txtCol.b, 195};
    SDL_Color secCol = {txtCol.r, txtCol.g, txtCol.b, 175};
    SDL_Color accentCol = btn.hasAccentColor
        ? SDL_Color{btn.accentR, btn.accentG, btn.accentB, 255}
        : SDL_Color{59, 130, 246, 255};

    std::time_t now = std::time(nullptr);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &now);
#else
    localtime_r(&now, &localTm);
#endif

    if (btn.clockIsAnalog) {
        int cx = drawRect.x + drawRect.w / 2;
        int cy = drawRect.y + drawRect.h / 2 - (btn.clockZoneText.empty() ? 0 : 8);
        int dialR = std::max(24, std::min(drawRect.w, drawRect.h - (btn.clockZoneText.empty() ? 16 : 32)) / 2 - 8);

        drawFilledCircleAA(renderer_, cx, cy, dialR, {20, 24, 32, 255}, {70, 80, 100, 180}, 2);
        for (int i = 0; i < 12; ++i) {
            double ang = (i * 30.0 - 90.0) * (3.1415926535 / 180.0);
            int r1 = dialR - ((i % 3 == 0) ? 8 : 5);
            int r2 = dialR - 2;
            int x1 = cx + static_cast<int>(std::round(std::cos(ang) * r1));
            int y1 = cy + static_cast<int>(std::round(std::sin(ang) * r1));
            int x2 = cx + static_cast<int>(std::round(std::cos(ang) * r2));
            int y2 = cy + static_cast<int>(std::round(std::sin(ang) * r2));
            thickLineRGBA(renderer_, x1, y1, x2, y2, (i % 3 == 0) ? 2 : 1, 200, 210, 225, 220);
        }

        double secFrac = localTm.tm_sec / 60.0;
        double minFrac = (localTm.tm_min + secFrac) / 60.0;
        double hourFrac = ((localTm.tm_hour % 12) + minFrac) / 12.0;

        double hAng = (hourFrac * 360.0 - 90.0) * (3.1415926535 / 180.0);
        double mAng = (minFrac * 360.0 - 90.0) * (3.1415926535 / 180.0);
        thickLineRGBA(renderer_, cx, cy,
                      cx + static_cast<int>(std::round(std::cos(hAng) * (dialR * 0.52))),
                      cy + static_cast<int>(std::round(std::sin(hAng) * (dialR * 0.52))),
                      3, txtCol.r, txtCol.g, txtCol.b, 255);
        thickLineRGBA(renderer_, cx, cy,
                      cx + static_cast<int>(std::round(std::cos(mAng) * (dialR * 0.74))),
                      cy + static_cast<int>(std::round(std::sin(mAng) * (dialR * 0.74))),
                      2, txtCol.r, txtCol.g, txtCol.b, 255);
        if (btn.clockShowSeconds) {
            double sAng = (secFrac * 360.0 - 90.0) * (3.1415926535 / 180.0);
            thickLineRGBA(renderer_, cx, cy,
                          cx + static_cast<int>(std::round(std::cos(sAng) * (dialR * 0.80))),
                          cy + static_cast<int>(std::round(std::sin(sAng) * (dialR * 0.80))),
                          1, accentCol.r, accentCol.g, accentCol.b, 255);
        }
        drawFilledCircleAA(renderer_, cx, cy, 4, accentCol);
        if (!btn.clockZoneText.empty()) {
            drawText(fontTiny_, btn.clockZoneText, cx, drawRect.y + drawRect.h - 14, subCol, true, drawRect.w - 16);
        }
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    } else {
        char hmBuf[16];
        std::snprintf(hmBuf, sizeof(hmBuf), "%02d:%02d", localTm.tm_hour, localTm.tm_min);
        char secBuf[16];
        std::snprintf(secBuf, sizeof(secBuf), ":%02d", localTm.tm_sec);

        std::string dateStr = btn.clockShowDate ? formatClockDate(localTm, "date-long", widgetCulture().rfind("tr", 0) == 0 ? 0 : widgetCulture().rfind("en", 0) == 0 ? 1 : config.language) : "";
        TTF_Font* timeFont = (drawRect.w >= 220 && fontXxl_) ? fontXxl_ : (fontXl_ ? fontXl_ : fontLarge_);
        TTF_Font* secFont  = (drawRect.w >= 220 && fontMedium_) ? fontMedium_ : fontNormal_;

        int cx = drawRect.x + drawRect.w / 2;
        int cy = drawRect.y + drawRect.h / 2;

        int timeCy = cy;
        int dateCy = cy;
        if (!dateStr.empty()) {
            if (btn.clockDatePosition == "above") {
                dateCy = cy - 28;
                timeCy = cy + 14;
            } else {
                timeCy = cy - 14;
                dateCy = cy + 28;
            }
        }

        if (!btn.clockZoneText.empty()) {
            drawText(fontTiny_, btn.clockZoneText, cx, drawRect.y + 16, subCol, true, drawRect.w - 16, false, false, true);
        }

        if (btn.clockShowSeconds) {
            int hmW = getTextWidth(timeFont, hmBuf);
            int secW = getTextWidth(secFont, secBuf);
            int totalW = hmW + secW + 2;
            int startX = cx - totalW / 2;
            drawText(timeFont, hmBuf, startX, timeCy, txtCol, false, 0, false, false, true);
            drawText(secFont, secBuf, startX + hmW + 2, timeCy + 6, secCol, false, 0, false, false, true);
        } else {
            drawText(timeFont, hmBuf, cx, timeCy, txtCol, true, drawRect.w - 16, false, false, true);
        }

        if (!dateStr.empty()) {
            drawText(fontNormal_, dateStr, cx, dateCy, subCol, true, drawRect.w - 16, false, false, true);
        }
    }
}

void UIRenderer::renderWeatherWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const DeckState& state, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color txtCol = (config.theme == 1) ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255};
    SDL_Color subCol = (config.theme == 1) ? SDL_Color{100, 116, 139, 255} : SDL_Color{168, 180, 198, 245};

    std::string tempStr = btn.weatherTemp.empty() ? btn.labelText : btn.weatherTemp;
    if (tempStr.empty()) tempStr = "--\xc2\xb0";
    else if (tempStr != "--\xc2\xb0" && tempStr != "--" && tempStr.find("\xc2\xb0") == std::string::npos) {
        tempStr += "\xc2\xb0";
    }
    std::string condStr = btn.weatherCondition;
    if (btn.weatherTemp.empty() && condStr.empty()) {
        condStr = config.language == 1 ? "Waiting for weather" : "Hava durumu bekleniyor";
    }
    std::string locStr  = btn.weatherLocation.empty() ? btn.thirdLabelText : btn.weatherLocation;
    std::string highLow = btn.weatherHighLow;
    if (highLow.empty()) highLow = "H: -- / L: --";
    if (config.language == 0) {
        const auto high = highLow.find("H:");
        const auto low = highLow.find("L:");
        if (high != std::string::npos) highLow[high] = 'Y';
        if (low != std::string::npos) highLow[low] = 'D';
    }
    std::string iconHint = btn.resourceId + " " + condStr;

    int padX = 14;
    int padY = 10;
    std::string st = btn.weatherStyle;
    for (char& ch : st) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    bool isCompactStyle = (st == "compact" || st == "kompakt");
    bool hasForecast = (!isCompactStyle && !btn.weatherForecast.empty() && drawRect.h >= 120 && drawRect.w >= 180);

    if (isCompactStyle && drawRect.w >= 180) {
        // Macro Deck 3 Weather Compact style (2x1 / wide):
        // Left: Location + large Temp | Right: Icon + Condition + High/Low
        int leftW = drawRect.w / 2 - padX;
        int curY = drawRect.y + padY + 4;
        if (!locStr.empty()) {
            drawText(fontSmall_, locStr, drawRect.x + padX, curY, txtCol, false, leftW);
        }
        TTF_Font* bigTempFont = (drawRect.h >= 110 && fontXl_) ? fontXl_ : fontLarge_;
        int tempCy = drawRect.y + drawRect.h - 28;
        drawText(bigTempFont, tempStr, drawRect.x + padX, tempCy, txtCol, false, leftW, false, false, true);

        int rightX = drawRect.x + drawRect.w - padX;
        int iconSize = std::clamp(drawRect.h / 3, 24, 36);
        int iconCy = drawRect.y + padY + iconSize / 2 + 2;
        int iconCx = rightX - iconSize / 2;
        if (btn.iconTexture && btn.iconWidth > 0 && btn.iconHeight > 0) {
            SDL_Rect icDst = { iconCx - iconSize / 2, iconCy - iconSize / 2, iconSize, iconSize };
            SDL_RenderCopy(renderer_, btn.iconTexture, nullptr, &icDst);
        } else {
            drawWeatherSymbol(renderer_, iconHint, iconCx, iconCy, iconSize);
        }
        int detailY = drawRect.y + drawRect.h - (highLow.empty() ? 22 : 34);
        if (!condStr.empty()) {
            drawText(fontSmall_, condStr, rightX, detailY, txtCol, false, leftW, true);
        }
        if (!highLow.empty()) {
            drawText(fontTiny_, highLow, rightX, detailY + 17, subCol, false, leftW, true);
        }
        return;
    }

    int forecastH = hasForecast ? std::min(drawRect.h / 2, static_cast<int>(btn.weatherForecast.size()) * 18 + 12) : 0;
    int topAreaH = drawRect.h - forecastH;

    if (drawRect.w >= 200) {
        // Macro Deck 3 Forecast / Head style: Icon + Temp on left, Condition + Location on right
        int iconSize = hasForecast ? 32 : 42;
        int iconCx = drawRect.x + padX + iconSize / 2;
        int iconCy = drawRect.y + topAreaH / 2;

        if (btn.iconTexture && btn.iconWidth > 0 && btn.iconHeight > 0) {
            SDL_Rect icDst = { iconCx - iconSize / 2, iconCy - iconSize / 2, iconSize, iconSize };
            SDL_RenderCopy(renderer_, btn.iconTexture, nullptr, &icDst);
        } else {
            drawWeatherSymbol(renderer_, iconHint, iconCx, iconCy, iconSize);
        }

        TTF_Font* tempFont = (hasForecast && drawRect.h < 160) ? fontLarge_ : (fontXl_ ? fontXl_ : fontLarge_);
        int tempX = iconCx + iconSize / 2 + 10;
        drawText(tempFont, tempStr, tempX, iconCy, txtCol, false, 110, false, false, true);

        int rightX = drawRect.x + drawRect.w - padX;
        int maxRightW = std::max(56, rightX - (tempX + getTextWidth(tempFont, tempStr) + 8));
        int infoTopY = drawRect.y + std::max(6, topAreaH / 2 - 18);

        if (!condStr.empty()) {
            drawText(fontSmall_, condStr, rightX, infoTopY, txtCol, false, maxRightW, true);
            infoTopY += 18;
        }
        if (!locStr.empty()) {
            drawText(fontTiny_, locStr, rightX, infoTopY, subCol, false, maxRightW, true);
            infoTopY += 15;
        }
        if (!highLow.empty()) {
            drawText(fontTiny_, highLow, rightX, infoTopY, subCol, false, maxRightW, true);
        }
    } else {
        const int cx = drawRect.x + drawRect.w / 2;
        const int detail = std::clamp(drawRect.h / 9, 8, 15);
        const int iconSize = std::max(8, std::min(drawRect.h * 28 / 100, drawRect.w / 2));
        const int iconY = drawRect.y + detail + 3 + iconSize / 2;
        auto* detailFont = widgetFont(detail, false);
        drawText(detailFont, locStr, cx, drawRect.y + detail / 2 + 4, subCol, true, drawRect.w - 10);
        if (btn.iconTexture && btn.iconWidth > 0 && btn.iconHeight > 0) {
            const SDL_Rect icon{cx - iconSize / 2, iconY - iconSize / 2, iconSize, iconSize};
            SDL_RenderCopy(renderer_, btn.iconTexture, nullptr, &icon);
        } else {
            drawWeatherSymbol(renderer_, iconHint, cx, iconY, iconSize);
        }
        drawText(widgetFont(std::clamp(drawRect.h / 5, 12, 36), true),
                 tempStr, cx, drawRect.y + drawRect.h * 57 / 100, txtCol, true, drawRect.w - 10);
        drawText(detailFont, condStr, cx, drawRect.y + drawRect.h * 75 / 100, txtCol, true, drawRect.w - 10);
        drawText(detailFont, highLow, cx, drawRect.y + drawRect.h * 89 / 100, subCol, true, drawRect.w - 10);
    }

    if (hasForecast) {
        int count = std::min(static_cast<int>(btn.weatherForecast.size()), 5);
        int fy = drawRect.y + drawRect.h - forecastH;
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 20);
        SDL_RenderDrawLine(renderer_, drawRect.x + padX, fy, drawRect.x + drawRect.w - padX, fy);

        int rowH = std::max(14, (forecastH - 6) / std::max(1, count));
        for (int i = 0; i < count; ++i) {
            const auto& fday = btn.weatherForecast[i];
            int ry = fy + 4 + i * rowH + rowH / 2;
            int rx = drawRect.x + padX;
            drawText(fontTiny_, fday.label, rx, ry, txtCol, false, 36, false, false, true);

            SDL_Texture* fTex = state.getResourceTexture(fday.resourceId);
            if (fTex) {
                SDL_Rect dst = { rx + 38, ry - 7, 14, 14 };
                SDL_RenderCopy(renderer_, fTex, nullptr, &dst);
            } else {
                drawWeatherSymbol(renderer_, fday.resourceId.empty() ? iconHint : fday.resourceId, rx + 45, ry, 13);
            }

            std::string minTxt = fday.minTemp;
            std::string maxTxt = fday.maxTemp;
            int minX = rx + 62;
            int maxX = drawRect.x + drawRect.w - padX;
            drawText(fontTiny_, minTxt, minX, ry, subCol, false, 32, false, false, true);
            drawText(fontTiny_, maxTxt, maxX, ry, txtCol, false, 32, true, false, true);

            int barX = minX + 34;
            int barW = std::max(20, (maxX - 34) - barX);
            SDL_Rect track = { barX, ry - 2, barW, 4 };
            drawRoundedBox(renderer_, track, 2, {255, 255, 255, 28});
            SDL_Rect fill = { barX + barW / 6, ry - 2, std::max(6, (barW * 2) / 3), 4 };
            drawRoundedBox(renderer_, fill, 2, {56, 189, 248, 230});
        }
    }
}

void UIRenderer::renderGaugesWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const DeckState& state, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color txtCol = (config.theme == 1) ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255};
    SDL_Color subCol = (config.theme == 1) ? SDL_Color{100, 116, 139, 255} : SDL_Color{165, 175, 190, 240};

    if (btn.gauges.empty()) {
        renderActionButtonWidget(btn, drawRect, radius, config);
        return;
    }

    const int count = static_cast<int>(btn.gauges.size());
    const int padX = std::clamp(drawRect.w / 14, 4, 14);
    const int padY = std::clamp(drawRect.h / 14, 4, 14);

    if (btn.gaugeIsArc) {
        int cols = 1;
        int bestSize = 0;
        for (int candidate = 1; candidate <= (btn.width == 1 && btn.height > 1 ? 1 : count); ++candidate) {
            const int rows = (count + candidate - 1) / candidate;
            const int size = std::min((drawRect.w - 2 * padX) / candidate, (drawRect.h - 2 * padY) / rows);
            if (size >= bestSize) {
                bestSize = size;
                cols = candidate;
            }
        }
        const int rows = (count + cols - 1) / cols;
        const int sliceW = std::max(1, (drawRect.w - 2 * padX) / cols);
        const int sliceH = std::max(1, (drawRect.h - 2 * padY) / rows);
        const bool hasNames = std::any_of(btn.gauges.begin(), btn.gauges.end(),
                                         [](const GaugeItem& gauge) { return !gauge.name.empty(); });
        const int labelSize = std::clamp(std::min(sliceW, sliceH) / 6, 8, 24);
        const int labelSpace = hasNames ? labelSize + 4 : 0;
        const int diameter = std::max(6, std::min(sliceW - 6, sliceH - labelSpace - 4));
        const int ringThick = std::clamp(diameter / 11, 2, 12);
        const int r = std::max(3, (diameter - ringThick - 4) / 2);
        for (int i = 0; i < count; ++i) {
            const auto& g = btn.gauges[i];
            const int rowCount = std::min(cols, count - (i / cols) * cols);
            const int cx = drawRect.x + (drawRect.w - rowCount * sliceW) / 2 + (i % cols) * sliceW + sliceW / 2;
            const int cy = drawRect.y + (drawRect.h - rows * sliceH) / 2 + (i / cols) * sliceH + (sliceH - labelSpace) / 2;

            SDL_Color gCol = g.hasColor ? SDL_Color{g.r, g.g, g.b, 255} : SDL_Color{59, 130, 246, 255};
            SDL_Color trackCol = (config.theme == 1) ? SDL_Color{203, 213, 225, 220} : SDL_Color{42, 48, 60, 255};

            // Smooth 270-degree circular gauge track & level fill (135 deg to 405 deg)
            drawSmoothArcAA(renderer_, cx, cy, r, ringThick, 135.0f, 405.0f, trackCol);
            float lvl = std::clamp(g.level, 0.0f, 1.0f);
            if (lvl > 0.005f) {
                drawSmoothArcAA(renderer_, cx, cy, r, ringThick, 135.0f, 135.0f + 270.0f * lvl, gCol);
            }

            // Radial segmented tick lines (10 ticks along 270-deg arc) matching Macro Deck hardware gauges
            const int numTicks = 10;
            for (int t = 0; t < numTicks; ++t) {
                float tFrac = static_cast<float>(t) / static_cast<float>(numTicks - 1);
                float angleDeg = 135.0f + 270.0f * tFrac;
                float rad = angleDeg * (3.14159265f / 180.0f);
                float cosA = std::cos(rad);
                float sinA = std::sin(rad);

                int rIn = r - ringThick / 2 - 2;
                int rOut = r + ringThick / 2 + 2;
                int x1 = cx + static_cast<int>(std::round(cosA * rIn));
                int y1 = cy + static_cast<int>(std::round(sinA * rIn));
                int x2 = cx + static_cast<int>(std::round(cosA * rOut));
                int y2 = cy + static_cast<int>(std::round(sinA * rOut));

                bool active = (lvl > 0.005f && tFrac <= lvl + 0.01f);
                if (active) {
                    thickLineRGBA(renderer_, x1, y1, x2, y2, 2, gCol.r, gCol.g, gCol.b, 255);
                } else {
                    thickLineRGBA(renderer_, x1, y1, x2, y2, 1, 35, 40, 50, 200);
                }
            }

            std::string valTxt = g.reading;
            if (valTxt.empty() || valTxt == "--") {
                valTxt = std::to_string(static_cast<int>(std::round(lvl * 100.0f))) + "%";
            }
            SDL_Texture* gIcon = state.getResourceTexture(g.resourceId);

            const int icSz = std::clamp(r * 2 / 3, 5, 34);
            const int valueSize = std::clamp(r * 3 / 5, 8, 34);
            const int icCy = cy - (valueSize + 2) / 2;
            const int textCy = cy + (icSz + 2) / 2;

            if (gIcon) {
                SDL_Rect icDst = { cx - icSz / 2, icCy - icSz / 2, icSz, icSz };
                SDL_RenderCopy(renderer_, gIcon, nullptr, &icDst);
            } else {
                std::string iconKey = "cpu";
                std::string lowerName = g.name;
                for (char& c : lowerName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lowerName.find("ram") != std::string::npos || lowerName.find("mem") != std::string::npos || i == 1) {
                    iconKey = "ram";
                } else if (lowerName.find("gpu") != std::string::npos || lowerName.find("vga") != std::string::npos || i == 2) {
                    iconKey = "gpu";
                }
                drawVectorIcon(iconKey, cx, icCy, icSz, gCol);
            }

            drawText(widgetFont(valueSize, true), valTxt, cx, textCy, txtCol, true,
                     std::max(1, diameter - 2 * ringThick));
            if (!g.name.empty()) {
                drawText(widgetFont(labelSize, false), g.name, cx, cy + (diameter + labelSpace) / 2,
                         subCol, true, sliceW - 4);
            }
        }
    } else {
        const int availH = drawRect.h - 2 * padY;
        const int rowH = std::max(1, availH / count);
        const int availW = drawRect.w - 2 * padX;
        const int labelSize = std::clamp(std::min(rowH / 2, availW / 8), 8, 26);
        const int barH = std::clamp(rowH / 5, 2, 13);
        const int groupTop = drawRect.y + (drawRect.h - count * rowH) / 2;

        for (int i = 0; i < count; ++i) {
            const auto& g = btn.gauges[i];
            const int rowY = groupTop + i * rowH + std::max(0, (rowH - labelSize - barH - 4) / 2);

            SDL_Color gCol = g.hasColor ? SDL_Color{g.r, g.g, g.b, 255} : SDL_Color{59, 130, 246, 255};
            std::string valTxt = g.reading.empty() ? (std::to_string(static_cast<int>(std::round(g.level * 100.0f))) + "%") : g.reading;

            const int iconSize = std::max(5, labelSize);
            int labelX = drawRect.x + padX;
            SDL_Texture* gIcon = state.getResourceTexture(g.resourceId);
            if (gIcon) {
                SDL_Rect icDst = { labelX, rowY, iconSize, iconSize };
                SDL_RenderCopy(renderer_, gIcon, nullptr, &icDst);
                labelX += iconSize + 4;
            } else {
                std::string iconKey = (i == 1) ? "ram" : ((i == 2) ? "gpu" : "cpu");
                drawVectorIcon(iconKey, labelX + iconSize / 2, rowY + iconSize / 2, iconSize, gCol);
                labelX += iconSize + 4;
            }

            const int valueW = std::min(availW / 2, getTextWidth(widgetFont(labelSize, true), valTxt));
            drawText(widgetFont(labelSize, false), g.name, labelX, rowY + labelSize / 2, txtCol, false,
                     std::max(1, availW - iconSize - valueW - 8), false, false, true);
            drawText(widgetFont(labelSize, true), valTxt, drawRect.x + drawRect.w - padX, rowY + labelSize / 2,
                     subCol, false, valueW, true, false, true);

            const int barY = rowY + labelSize + 4;
            SDL_Rect trackRect = { drawRect.x + padX, barY, availW, barH };
            drawRoundedBox(renderer_, trackRect, 4, {38, 44, 56, 255});

            int fillW = static_cast<int>(std::round(availW * std::clamp(g.level, 0.0f, 1.0f)));
            if (fillW > 0) {
                SDL_Rect fillRect = { drawRect.x + padX, barY, std::max(barH, fillW), barH };
                drawRoundedBox(renderer_, fillRect, 4, gCol);
            }
        }
    }
}

void UIRenderer::renderCountdownWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    if (btn.countdownIsFinished && !btn.hasBgColor) {
        bgCol = {198, 40, 40, 255};
    }
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color txtCol = (config.theme == 1 && !btn.countdownIsFinished) ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255};

    Uint64 nowTicks = static_cast<Uint64>(SDL_GetTicks());
    double posMs = btn.getCurrentProgressMs(nowTicks, true);
    bool isPaused = btn.countdownIsPaused && !btn.countdownIsFinished && btn.progressRate == 0.0;

    SDL_Color accentCol;
    if (btn.countdownIsFinished) {
        float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(nowTicks) * 0.008f);
        Uint8 r = static_cast<Uint8>(std::round(239.0f * pulse + 198.0f * (1.0f - pulse)));
        accentCol = {r, 56, 56, 255};
    } else if (isPaused) {
        accentCol = {138, 138, 138, 255}; // Macro Deck 3 #8a8a8a paused grey
    } else {
        accentCol = btn.hasAccentColor
            ? SDL_Color{btn.accentR, btn.accentG, btn.accentB, 255}
            : SDL_Color{33, 150, 243, 255};
    }

    std::string timeStr = btn.countdownTimeText;
    if (timeStr.empty()) timeStr = btn.labelText.empty() ? "0:00" : btn.labelText;

    if (btn.progressDurationMs > 0.0) {
        double remMs = std::max(0.0, btn.progressDurationMs - posMs);
        timeStr = btn.countdownReadout(nowTicks);
        if (remMs <= 0.0 && btn.progressRate > 0.0) {
            const_cast<DeckButton&>(btn).countdownIsFinished = true;
        }
    }

    if (btn.countdownIsIdle()) timeStr = btn.countdownReadout(nowTicks);

    float frac = btn.countdownIsFinished ? 1.0f
               : ((btn.progressDurationMs > 0.0) ? (1.0f - btn.getCurrentProgressFraction(nowTicks)) : btn.countdownRingLevel);
    frac = std::clamp(frac, 0.0f, 1.0f);

    int cx = drawRect.x + drawRect.w / 2;
    int cy = drawRect.y + drawRect.h / 2;
    int r = std::max(28, std::min(drawRect.w, drawRect.h) / 2 - 12);
    int ringThick = std::clamp(r / 5, 5, 8);

    drawSmoothArcAA(renderer_, cx, cy, r, ringThick, -90.0f, 270.0f, {42, 48, 60, 255});
    if (frac > 0.005f) {
        drawSmoothArcAA(renderer_, cx, cy, r, ringThick, -90.0f, -90.0f + 360.0f * frac, accentCol);
    }

    TTF_Font* timeFont = (r >= 36 && fontMedium_) ? fontMedium_ : fontNormal_;
    drawText(timeFont, timeStr, cx, cy, txtCol, true, r * 2 - 4, false, false, true);
}

void UIRenderer::renderBatteryWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, int systemBatteryPct, bool systemIsCharging, const DeckState& state, const AppConfig& config) {
    (void)systemBatteryPct;
    (void)systemIsCharging;
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color txtCol = (config.theme == 1) ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255};
    SDL_Color subCol = (config.theme == 1) ? SDL_Color{100, 116, 139, 255} : SDL_Color{165, 175, 190, 240};

    const auto drawDeviceIcon = [&](const BatteryDeviceItem& dev, int x, int y, int size) {
        const auto& cacheKey = dev.resourceId;
        if (auto* texture = state.getResourceTexture(cacheKey, nullptr, nullptr, SDL_GetTicks64())) {
            const SDL_Rect icon{x - size / 2, y - size / 2, size, size};
            SDL_RenderCopy(renderer_, texture, nullptr, &icon);
        } else {
            drawVectorIcon(dev.iconName.empty() ? (dev.isCharging ? "battery-charging" : "battery") : dev.iconName,
                           x, y, size, txtCol);
        }
    };

    if (btn.batteryDevices.empty()) {
        std::string msg = !btn.batteryStateText.empty() ? btn.batteryStateText
                        : (config.language == 1 ? "No battery" : "Cihaz Yok");
        drawVectorIcon("battery", drawRect.x + drawRect.w / 2, drawRect.y + drawRect.h / 2 - 14, 24, subCol);
        drawText(fontTiny_, msg, drawRect.x + drawRect.w / 2, drawRect.y + drawRect.h / 2 + 16, subCol, true, drawRect.w - 14);
        return;
    }

    if (btn.batteryDevices.size() == 1) {
        const auto& dev = btn.batteryDevices[0];
        int batPct = std::clamp(static_cast<int>(std::round(dev.level * 100.0f)), 0, 100);
        SDL_Color accentCol = dev.hasColor ? SDL_Color{dev.r, dev.g, dev.b, 255}
                            : (dev.isCharging ? SDL_Color{34, 197, 94, 255}
                            : (batPct <= 15 ? SDL_Color{239, 68, 68, 255}
                            : (batPct <= 30 ? SDL_Color{245, 158, 11, 255} : SDL_Color{34, 197, 94, 255})));

        bool hasLabel = !dev.name.empty() || !dev.stateText.empty();
        int cx = drawRect.x + drawRect.w / 2;
        int cy = drawRect.y + drawRect.h / 2 - (hasLabel ? 10 : 0);
        int r = std::max(20, std::min(drawRect.w, drawRect.h - (hasLabel ? 26 : 8)) / 2 - 10);
        int ringThick = std::clamp(r / 5, 4, 7);

        drawSmoothArcAA(renderer_, cx, cy, r, ringThick, 0.0f, 360.0f, {42, 48, 60, 255});
        if (dev.level > 0.005f) {
            drawSmoothArcAA(renderer_, cx, cy, r, ringThick, -90.0f, -90.0f + 360.0f * std::clamp(dev.level, 0.0f, 1.0f), accentCol);
        }

        drawDeviceIcon(dev, cx, cy - 8, std::clamp(r / 2, 10, 22));
        const auto pctStr = dev.valueText.empty() ? (std::to_string(batPct) + "%") : dev.valueText;
        drawText(fontTiny_, pctStr, cx, cy + 10, txtCol, true, r * 2 - 4, false, false, true);

        if (dev.isCharging) {
            filledTrigonRGBA(renderer_, cx + r - 3, cy - r + 2, cx + r - 8, cy - r + 8, cx + r - 4, cy - r + 8, 34, 197, 94, 255);
            filledTrigonRGBA(renderer_, cx + r - 6, cy - r + 7, cx + r - 1, cy - r + 7, cx + r - 7, cy - r + 14, 34, 197, 94, 255);
        }

        std::string bottomLbl = !dev.name.empty() ? dev.name : dev.stateText;
        if (!bottomLbl.empty()) {
            drawText(fontTiny_, bottomLbl, cx, drawRect.y + drawRect.h - 14, subCol, true, drawRect.w - 10, false, false, true);
        }
        return;
    }

    // Multiple PC peripherals (grid or compact rows depending on count & tile size)
    const int count = std::min(static_cast<int>(btn.batteryDevices.size()), 8);
    const int pad = std::clamp(std::min(drawRect.w, drawRect.h) / 14, 3, 8);
    int cols = 1;
    int bestSize = 0;
    for (int candidate = 1; candidate <= count; ++candidate) {
        const int rows = (count + candidate - 1) / candidate;
        const int size = std::min((drawRect.w - 2 * pad) / candidate, (drawRect.h - 2 * pad) / rows);
        if (size > bestSize) {
            bestSize = size;
            cols = candidate;
        }
    }
    const int rows = (count + cols - 1) / cols;
    int cellW = (drawRect.w - 2 * pad) / cols;
    int cellH = (drawRect.h - 2 * pad) / rows;

    for (int i = 0; i < count; ++i) {
        const auto& dev = btn.batteryDevices[i];
        int rIdx = i / cols;
        int cIdx = i % cols;
        const bool showName = !dev.name.empty() && cellH >= 38;
        const int nameHeight = showName ? std::clamp(cellH / 6, 8, 14) : 0;
        const int rowCount = std::min(cols, count - rIdx * cols);
        const int cx = drawRect.x + (drawRect.w - rowCount * cellW) / 2 + cIdx * cellW + cellW / 2;
        const int cy = drawRect.y + pad + rIdx * cellH + (cellH - nameHeight) / 2;
        const int r = std::max(4, (std::min(cellW, cellH - nameHeight) - 6) / 2);
        int thick = std::max(3, r / 4);

        SDL_Color accentCol = dev.hasColor ? SDL_Color{dev.r, dev.g, dev.b, 255} : SDL_Color{34, 197, 94, 255};
        drawSmoothArcAA(renderer_, cx, cy, r, thick, 0.0f, 360.0f, {42, 48, 60, 255});
        if (dev.level > 0.005f) {
            drawSmoothArcAA(renderer_, cx, cy, r, thick, -90.0f, -90.0f + 360.0f * std::clamp(dev.level, 0.0f, 1.0f), accentCol);
        }
        std::string vTxt = dev.valueText.empty() ? (std::to_string(static_cast<int>(std::round(dev.level * 100.0f))) + "%") : dev.valueText;
        const int iconSize = std::clamp(r / 2, 7, 17);
        drawDeviceIcon(dev, cx, cy - iconSize / 2, iconSize);
        drawText(widgetFont(std::clamp(r / 2, 8, 13), false), vTxt, cx, cy + iconSize / 2,
                 txtCol, true, r * 2 - 2, false, false, true);
        if (showName) {
            drawText(widgetFont(nameHeight, false), dev.name, cx, drawRect.y + pad + (rIdx + 1) * cellH - nameHeight / 2,
                     subCol, true, cellW - 4);
        }
    }
}

void UIRenderer::renderStopwatchWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color txtCol = (config.theme == 1) ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255};

    Uint64 nowTicks = static_cast<Uint64>(SDL_GetTicks());
    double curMs = btn.getCurrentProgressMs(nowTicks, false);
    bool isPaused = btn.stopwatchIsPaused || (btn.progressRate == 0.0 && curMs > 0.0);

    SDL_Color accentCol = isPaused
        ? SDL_Color{138, 138, 138, 255}
        : (btn.hasAccentColor ? SDL_Color{btn.accentR, btn.accentG, btn.accentB, 255} : SDL_Color{33, 150, 243, 255});

    std::string timeStr;
    if (curMs > 0.0 || btn.progressRate > 0.0) {
        timeStr = DeckButton::formatDurationMs(curMs);
    } else if (!btn.stopwatchTimeText.empty()) {
        timeStr = btn.stopwatchTimeText;
    } else {
        timeStr = "0:00";
    }

    float totalSec = static_cast<float>(curMs / 1000.0);
    float frac = fmod(totalSec, 60.0f) / 60.0f;
    if (totalSec > 0.0f && frac == 0.0f) frac = 1.0f;
    frac = std::clamp(frac, 0.0f, 1.0f);

    int cx = drawRect.x + drawRect.w / 2;
    int cy = drawRect.y + drawRect.h / 2;
    int r = std::max(28, std::min(drawRect.w, drawRect.h) / 2 - 12);
    int ringThick = std::clamp(r / 5, 5, 8);

    drawSmoothArcAA(renderer_, cx, cy, r, ringThick, -90.0f, 270.0f, {42, 48, 60, 255});
    if (frac > 0.002f) {
        drawSmoothArcAA(renderer_, cx, cy, r, ringThick, -90.0f, -90.0f + 360.0f * frac, accentCol);
    }

    TTF_Font* timeFont = (r >= 36 && fontMedium_) ? fontMedium_ : fontNormal_;
    drawText(timeFont, timeStr, cx, cy, txtCol, true, r * 2 - 4, false, false, true);
}

void UIRenderer::renderCalendarWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color txtCol = (config.theme == 1) ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255};
    SDL_Color subCol = (config.theme == 1) ? SDL_Color{100, 116, 139, 255} : SDL_Color{165, 175, 190, 240};

    // If Macro Deck 3 Agenda events or NextEvent details exist, display them cleanly
    if (!btn.calendarEvents.empty() || !btn.calendarNextTitle.empty()) {
        int pad = 12;
        int curY = drawRect.y + pad;
        if (!btn.calendarWeekdayText.empty() || !btn.calendarDayText.empty()) {
            drawText(fontTiny_, btn.calendarWeekdayText, drawRect.x + pad, curY, {255, 59, 48, 255}, false, drawRect.w - 2 * pad);
            curY += 16;
            drawText(fontMedium_, btn.calendarDayText, drawRect.x + pad, curY, txtCol, false, drawRect.w - 2 * pad);
            curY += 26;
        } else if (!btn.calendarNextDate.empty()) {
            drawText(fontTiny_, btn.calendarNextDate, drawRect.x + pad, curY, {255, 59, 48, 255}, false, drawRect.w - 2 * pad);
            curY += 16;
        }

        if (!btn.calendarEvents.empty()) {
            for (size_t i = 0; i < btn.calendarEvents.size() && curY + 20 < drawRect.y + drawRect.h - 4; ++i) {
                const auto& ev = btn.calendarEvents[i];
                drawRoundedBox(renderer_, {drawRect.x + pad, curY + 2, 3, 24}, 1, {59, 130, 246, 255});
                drawText(fontSmall_, ev.title, drawRect.x + pad + 8, curY, txtCol, false, drawRect.w - 2 * pad - 10);
                if (!ev.timeText.empty()) {
                    drawText(fontTiny_, ev.timeText, drawRect.x + pad + 8, curY + 15, subCol, false, drawRect.w - 2 * pad - 10);
                    curY += 32;
                } else {
                    curY += 20;
                }
            }
        } else if (!btn.calendarNextTitle.empty()) {
            drawRoundedBox(renderer_, {drawRect.x + pad, curY + 2, 3, 30}, 1, {59, 130, 246, 255});
            drawText(fontNormal_, btn.calendarNextTitle, drawRect.x + pad + 8, curY, txtCol, false, drawRect.w - 2 * pad - 10);
            if (!btn.calendarNextWhen.empty()) {
                drawText(fontTiny_, btn.calendarNextWhen, drawRect.x + pad + 8, curY + 20, subCol, false, drawRect.w - 2 * pad - 10);
            }
        }
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &now);
#else
    localtime_r(&now, &localTm);
#endif

    static const char* trMonths[] = {"OCAK", "ŞUBAT", "MART", "NİSAN", "MAYIS", "HAZİRAN", "TEMMUZ", "AĞUSTOS", "EYLÜL", "EKİM", "KASIM", "ARALIK"};
    static const char* trDays[]   = {"Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"};
    static const char* enMonths[] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
    static const char* enDays[]   = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

    int mon = std::clamp(localTm.tm_mon, 0, 11);
    int wday = std::clamp(localTm.tm_wday, 0, 6);

    std::string monthStr = btn.calendarMonthText.empty() ? (config.language == 1 ? enMonths[mon] : trMonths[mon]) : btn.calendarMonthText;
    std::string dayStr = btn.calendarDayText.empty() ? std::to_string(localTm.tm_mday) : btn.calendarDayText;
    std::string wdayStr = btn.calendarWeekdayText.empty() ? (config.language == 1 ? enDays[wday] : trDays[wday]) : btn.calendarWeekdayText;

    // Top Red Header Banner
    int headerH = std::max(24, static_cast<int>(drawRect.h * 0.28f));
    SDL_Rect headerRect = { drawRect.x, drawRect.y, drawRect.w, headerH };
    drawRoundedBox(renderer_, headerRect, radius, {239, 68, 68, 255});
    boxRGBA(renderer_, drawRect.x, drawRect.y + headerH - radius, drawRect.x + drawRect.w - 1, drawRect.y + headerH - 1, 239, 68, 68, 255);

    drawText(fontSmall_, monthStr, drawRect.x + drawRect.w / 2, drawRect.y + headerH / 2, {255, 255, 255, 255}, true, drawRect.w - 16, false, false, true);

    TTF_Font* dayFont = (drawRect.h >= 110 && fontXl_) ? fontXl_ : fontLarge_;
    int bodyH = drawRect.h - headerH;
    int dayCy = drawRect.y + headerH + bodyH / 2 - 8;
    drawText(dayFont, dayStr, drawRect.x + drawRect.w / 2, dayCy, txtCol, true, drawRect.w - 16, false, false, true);
    drawText(fontTiny_, wdayStr, drawRect.x + drawRect.w / 2, drawRect.y + drawRect.h - 14, subCol, true, drawRect.w - 12, false, false, true);
}

void UIRenderer::renderHistoryGraphWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    SDL_Color txtCol = (config.theme == 1) ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255};
    SDL_Color accentCol = btn.hasAccentColor
        ? SDL_Color{btn.accentR, btn.accentG, btn.accentB, 255}
        : SDL_Color{59, 130, 246, 255};

    int padX = 14;
    int padY = 10;

    std::string titleStr = btn.graphTitle.empty() ? (btn.labelText.empty() ? "Usage" : btn.labelText) : btn.graphTitle;
    std::string valStr = btn.sliderValueText;
    if (valStr.empty()) {
        if (!btn.graphHistory.empty()) {
            int pct = static_cast<int>(std::round(btn.graphHistory.back() * 100.0f));
            valStr = std::to_string(pct) + " %";
        } else if (btn.sliderLevel > 0.0f) {
            int pct = static_cast<int>(std::round(btn.sliderLevel * 100.0f));
            valStr = std::to_string(pct) + " %";
        }
    }

    // Top-left Title
    drawText(fontSmall_, titleStr, drawRect.x + padX, drawRect.y + padY + 2, txtCol, false, drawRect.w - 2 * padX - 60, false, false, true);

    // Top-right Value Reading ("24 %")
    if (!valStr.empty()) {
        drawText(fontNormal_, valStr, drawRect.x + drawRect.w - padX, drawRect.y + padY + 2, accentCol, false, 68, true, false, true);
    }

    int graphX = drawRect.x + padX;
    int graphY = drawRect.y + 34;
    int graphW = drawRect.w - 2 * padX;
    int graphH = drawRect.h - 44;

    if (graphW > 10 && graphH > 10) {
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 20);
        SDL_RenderDrawLine(renderer_, graphX, graphY + graphH, graphX + graphW, graphY + graphH);

        const auto& hist = btn.graphHistory;
        int ptCount = static_cast<int>(hist.size());
        if (ptCount >= 2) {
            float stepX = static_cast<float>(graphW) / (ptCount - 1);
            std::vector<Sint16> vx(ptCount);
            std::vector<Sint16> vy(ptCount);
            for (int i = 0; i < ptCount; ++i) {
                vx[i] = static_cast<Sint16>(graphX + std::round(i * stepX));
                float val = std::clamp(hist[i], 0.0f, 1.0f);
                vy[i] = static_cast<Sint16>(graphY + graphH - std::round(val * graphH));
            }
            std::vector<Sint16> polyX = vx;
            std::vector<Sint16> polyY = vy;
            polyX.push_back(graphX + graphW); polyY.push_back(graphY + graphH);
            polyX.push_back(graphX);           polyY.push_back(graphY + graphH);
            filledPolygonRGBA(renderer_, polyX.data(), polyY.data(), static_cast<int>(polyX.size()), accentCol.r, accentCol.g, accentCol.b, 45);

            for (int i = 0; i < ptCount - 1; ++i) {
                thickLineRGBA(renderer_, vx[i], vy[i], vx[i + 1], vy[i + 1], 2, accentCol.r, accentCol.g, accentCol.b, 255);
            }
        } else {
            float t = static_cast<float>(SDL_GetTicks()) / 1000.0f;
            int numPts = 16;
            float stepX = static_cast<float>(graphW) / (numPts - 1);
            std::vector<Sint16> vx(numPts);
            std::vector<Sint16> vy(numPts);
            for (int i = 0; i < numPts; ++i) {
                vx[i] = static_cast<Sint16>(graphX + std::round(i * stepX));
                float wave = 0.35f + 0.25f * std::sin(t * 1.5f + i * 0.45f) + 0.15f * std::cos(t * 2.8f + i * 0.9f);
                wave = std::clamp(wave, 0.05f, 0.95f);
                vy[i] = static_cast<Sint16>(graphY + graphH - std::round(wave * graphH));
            }
            std::vector<Sint16> polyX = vx;
            std::vector<Sint16> polyY = vy;
            polyX.push_back(graphX + graphW); polyY.push_back(graphY + graphH);
            polyX.push_back(graphX);           polyY.push_back(graphY + graphH);
            filledPolygonRGBA(renderer_, polyX.data(), polyY.data(), static_cast<int>(polyX.size()), accentCol.r, accentCol.g, accentCol.b, 40);

            for (int i = 0; i < numPts - 1; ++i) {
                thickLineRGBA(renderer_, vx[i], vy[i], vx[i + 1], vy[i + 1], 2, accentCol.r, accentCol.g, accentCol.b, 255);
            }
        }
    }
}

void UIRenderer::renderActionButtonWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config) {
    SDL_Color bgCol = getVisibleTileBg(btn, config.theme);
    drawRoundedBox(renderer_, drawRect, radius, bgCol);

    bool hasText = !btn.labelText.empty() || !btn.subLabelText.empty();
    bool hasIcon = (btn.iconTexture != nullptr && btn.iconWidth > 0 && btn.iconHeight > 0);

    // Apply icon fit, zoom, offset, and opacity across full drawRect (matching Macro Deck 3 paintButtonArtwork 1:1)
    if (hasIcon) {
        if (btn.iconOpacity < 100) {
            SDL_SetTextureAlphaMod(btn.iconTexture, static_cast<Uint8>((btn.iconOpacity * 255) / 100));
        } else {
            SDL_SetTextureAlphaMod(btn.iconTexture, 255);
        }

        float scaleX = static_cast<float>(drawRect.w) / btn.iconWidth;
        float scaleY = static_cast<float>(drawRect.h) / btn.iconHeight;
        float scale = (btn.iconFit == "cover") ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
        if (btn.iconZoom != 100) scale *= (btn.iconZoom / 100.0f);

        int dstW = std::max(1, static_cast<int>(std::round(btn.iconWidth * scale)));
        int dstH = std::max(1, static_cast<int>(std::round(btn.iconHeight * scale)));
        int offsetX = (btn.iconOffsetX * drawRect.w) / 100;
        int offsetY = (btn.iconOffsetY * drawRect.h) / 100;

        SDL_Rect dst = {
            drawRect.x + (drawRect.w - dstW) / 2 + offsetX,
            drawRect.y + (drawRect.h - dstH) / 2 + offsetY,
            dstW,
            dstH
        };
        SDL_RenderSetClipRect(renderer_, &drawRect);
        SDL_RenderCopy(renderer_, btn.iconTexture, nullptr, &dst);
        if (radius > 1) {
            maskRoundedCorners(renderer_, drawRect, radius, bgCol);
        }
        SDL_RenderSetClipRect(renderer_, nullptr);
    } else if (!btn.iconName.empty()) {
        int iconCy = hasText ? (drawRect.y + drawRect.h / 2 - 14) : (drawRect.y + drawRect.h / 2);
        drawVectorIcon(btn.iconName, drawRect.x + drawRect.w / 2, iconCy, 28, {230, 235, 245, 240});
    } else if (!btn.navCommand.empty()) {
        int iconCy = hasText ? (drawRect.y + drawRect.h / 2 - 14) : (drawRect.y + drawRect.h / 2);
        std::string navIcon = (btn.navCommand == "back" || btn.navCommand == "parent") ? "back" : "folder";
        drawVectorIcon(navIcon, drawRect.x + drawRect.w / 2, iconCy, 26, {220, 220, 220, 235});
    }

    if (btn.labelTexture) {
        SDL_RenderCopy(renderer_, btn.labelTexture, nullptr, &drawRect);
        return;
    }

    if (hasText) {
        SDL_Color txtCol = btn.hasTextColor
            ? SDL_Color{btn.textR, btn.textG, btn.textB, 255}
            : (config.theme == 1 ? SDL_Color{15, 23, 42, 255} : SDL_Color{255, 255, 255, 255});

        int padX = 10;
        int padY = 8;
        SDL_Rect textArea = { drawRect.x + padX, drawRect.y + padY, drawRect.w - 2 * padX, drawRect.h - 2 * padY };

        if (!btn.subLabelText.empty()) {
            TTF_Font* mainFont = (btn.labelText.size() <= 8 && drawRect.w >= 110) ? fontLarge_ : fontNormal_;
            int mainH = TTF_FontLineSkip(mainFont);
            int subH = TTF_FontLineSkip(fontSmall_);
            int thirdH = btn.thirdLabelText.empty() ? 0 : TTF_FontLineSkip(fontTiny_);
            int totalH = mainH + subH + thirdH;
            int curY = drawRect.y + (drawRect.h - totalH) / 2;

            drawText(mainFont, btn.labelText, drawRect.x + drawRect.w / 2, curY + mainH / 2, txtCol, true, textArea.w, false, hasIcon || btn.labelShadow);
            curY += mainH;
            drawText(fontSmall_, btn.subLabelText, drawRect.x + drawRect.w / 2, curY + subH / 2, {200, 200, 200, 240}, true, textArea.w, false, hasIcon || btn.labelShadow);
            if (!btn.thirdLabelText.empty()) {
                curY += subH;
                drawText(fontTiny_, btn.thirdLabelText, drawRect.x + drawRect.w / 2, curY + thirdH / 2, {165, 165, 165, 220}, true, textArea.w, false, hasIcon || btn.labelShadow);
            }
        } else {
            const double scale = std::min(drawRect.w, drawRect.h) / 120.0;
            TTF_Font* chosenFont = widgetFont(static_cast<int>(std::lround(std::clamp(btn.fontSize, 4, 160) * scale)), false);

            std::string vPos = btn.labelPosition.empty() ? "center" : btn.labelPosition;
            drawWrappedText(chosenFont, btn.labelText, textArea, vPos, btn.textAlign, txtCol, hasIcon || btn.labelShadow);
        }
    }
}

void UIRenderer::drawVectorIcon(const std::string& iconName, int cx, int cy, int size, SDL_Color color) {
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
