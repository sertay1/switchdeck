#pragma once
#include <ctime>
#include <string>

std::string formatClockDate(const std::tm& date, const std::string& format, int language);
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "config.hpp"
#include "deck_state.hpp"
#include "ws_client.hpp"
#include "settings_ui.hpp"
#include <optional>
#include <tuple>

class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

    bool init();
    bool hasWidgetFont(const std::string& face) const;
    void setWidgetFontBytes(const std::string& face, const std::string& bytes);
    void clearWidgetFontBytes();
    void setDefaultWidgetFonts(const std::string& regular, const std::string& bold);
    void render(const AppConfig& config,
                const DeckState& state,
                WsState wsState,
                int pingMs,
                const std::string& statusMsg,
                bool isDiscovering,
                const std::string& discoveryStatus,
                int batteryPct,
                bool isCharging,
                int dimAlpha = 0,
                bool showUpdateModal = false,
                const std::string& newUpdateVersion = "",
                const std::string& actionError = "");
    void shutdown();

    SDL_Renderer* getRenderer() const { return renderer_; }
    TTF_Font* getFontTiny() const { return fontTiny_; }
    TTF_Font* getFontSmall() const { return fontSmall_; }
    TTF_Font* getFontNormal() const { return fontNormal_; }
    TTF_Font* getFontMedium() const { return fontMedium_; }
    TTF_Font* getFontLarge() const { return fontLarge_; }
    TTF_Font* getFontXl() const { return fontXl_; }
    TTF_Font* getFontXxl() const { return fontXxl_; }

    SDL_Rect getCellRect(int row, int col, int spanW, int spanH, const DeckState& state, const AppConfig& config) const;
    int getTileCornerRadius(int cellSize, const DeckState& state, const AppConfig& config) const;

    bool getButtonAt(int screenX, int screenY, const DeckState& state, const AppConfig& config, int& outRow, int& outCol) const;
    float getSliderLevelAt(int screenX, int screenY, const DeckButton& btn, const DeckState& state, const AppConfig& config) const;
    int getMusicSubButtonAt(int screenX, int screenY, const DeckButton& btn, const DeckState& state, const AppConfig& config) const;

    struct LevelControl {
        std::string id;
        SDL_Rect area{};
        bool vertical = false;
        bool relative = false;
        bool dial = false;
        bool adjusts = false;
        bool changes = false;
        bool doublePress = false;
        float level = 0;
        float step = 0;
        double startAngle = -135;
        double sweep = 270;

        float snap(float value) const;
        float valueAt(SDL_Point pointer, SDL_Point previous, float previousLevel, bool beginning) const;
    };

    std::optional<LevelControl> getTreeLevelControlAt(int x, int y, const DeckButton& btn,
                                                       const DeckState& state, const AppConfig& config);
    std::optional<std::string> getWidgetTreeButtonAt(int x, int y, const DeckButton& btn, const DeckState& state, const AppConfig& config);

    bool isSettingsButtonTouched(int screenX, int screenY, const AppConfig& config) const;
    bool isBackButtonTouched(int screenX, int screenY, const DeckState& state) const;
    bool isFolderPrevTouched(int screenX, int screenY, const DeckState& state) const;
    bool isFolderNextTouched(int screenX, int screenY, const DeckState& state) const;
    bool isUpdateModalDismissTouched(int screenX, int screenY) const;

    void moveFocus(int dRow, int dCol, int maxRows, int maxCols);
    void setFocus(int row, int col);
    void setShowFocusHighlight(bool show) { showFocusHighlight_ = show; }
    int getFocusRow() const { return focusRow_; }
    int getFocusCol() const { return focusCol_; }

    SettingsUI& getSettingsUI() { return settingsUI_; }

    // Shared Anti-Aliased Drawing Primitives & Hardware Text Cache
    static void drawRoundedBox(SDL_Renderer* r, const SDL_Rect& rect, int radius, SDL_Color color);
    static void drawRoundedBorder(SDL_Renderer* r, const SDL_Rect& rect, int radius, int thickness, SDL_Color color);
    static void drawFilledCircleAA(SDL_Renderer* r, int cx, int cy, int radius, SDL_Color fill, SDL_Color border = {0, 0, 0, 0}, int borderThickness = 0);
    static SDL_Texture* getCachedTextTexture(SDL_Renderer* r, TTF_Font* font, const std::string& text, int& outW, int& outH);
    static void clearRenderCaches();

    // Shared Status Widgets (identical between Main Menu and Settings)
    static void drawConnectionPill(SDL_Renderer* r, TTF_Font* fontSmall, TTF_Font* fontTiny, const SDL_Rect& rect, WsState wsState, int pingMs, bool isLight, bool isOled = false, int language = 0);
    static void drawWordmark(SDL_Renderer* r, TTF_Font* titleFont, TTF_Font* signatureFont, int x, int centerY,
                             SDL_Color titleColor, bool isLight);
    static void drawBatteryPill(SDL_Renderer* r, TTF_Font* fontSmall, const SDL_Rect& rect, int batteryPct, bool isCharging, int lowThreshold, bool isLight, bool isOled = false, int language = 0);

private:
    struct WidgetTree;
    static void drawSmoothArcAA(SDL_Renderer* r, int cx, int cy, int radius, int thickness,
                                float startDeg, float endDeg, SDL_Color color);
    static void maskRoundedCorners(SDL_Renderer* renderer, const SDL_Rect& rect, int radius, SDL_Color outerBg);
    struct ArtworkTransition {
        std::string shownId;
        std::string previousId;
        nlohmann::json shownStyle;
        nlohmann::json previousStyle;
        Uint64 started = 0;
        Uint64 lastSeen = 0;
    };
    std::map<std::pair<std::string, std::string>, ArtworkTransition> artworkTransitions_;
    std::map<std::tuple<int, bool, std::string>, TTF_Font*> widgetFonts_;
    std::map<std::string, std::string> widgetFontBytes_;
    size_t widgetFontBytesTotal_ = 0;
    std::string defaultWidgetFont_;
    std::string defaultBoldWidgetFont_;
    std::string widgetFontPath_;
    TTF_Font* widgetFont(int size, bool bold, const std::string& face = {});
    bool renderWidgetTree(const DeckButton& btn, const SDL_Rect& rect,
                          const DeckState& state, const AppConfig& config, int radius, bool isFocused);
    void renderWidgetBorder(const DeckButton& btn, const SDL_Rect& rect, int radius,
                            const DeckState& state, const AppConfig& config);
    void renderTopStatusBar(const AppConfig& config,
                            const DeckState& state,
                            WsState wsState,
                            int pingMs,
                            const std::string& statusMsg,
                            int batteryPct,
                            bool isCharging);
    void renderFloatingControls(const DeckState& state, int language);
    void renderPageControls(const DeckState& state, SDL_Color background, SDL_Color border,
                            SDL_Color text, SDL_Color arrow);
    void renderEmptyCell(const SDL_Rect& rect, int radius, bool isFocused, int theme);
    void renderWidgetTile(const DeckButton& btn, const SDL_Rect& rect, int radius, bool isFocused, const DeckState& state, const AppConfig& config, int batteryPct = 100, bool isCharging = false);
    void renderMusicPlayerWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config);
    void renderSliderWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config);
    void renderClockWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config);
    void renderWeatherWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const DeckState& state, const AppConfig& config);
    void renderGaugesWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const DeckState& state, const AppConfig& config);
    void renderCountdownWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config);
    void renderBatteryWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, int systemBatteryPct, bool systemIsCharging, const DeckState& state, const AppConfig& config);
    void renderStopwatchWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config);
    void renderCalendarWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config);
    void renderHistoryGraphWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config);
    void renderActionButtonWidget(const DeckButton& btn, const SDL_Rect& drawRect, int radius, const AppConfig& config);
    void renderUpdateModal(const std::string& newVersion);

    SDL_Color currentCanvasBg_ = {18, 18, 18, 255};

    void drawVectorIcon(const std::string& iconName, int cx, int cy, int size, SDL_Color color);
    void drawProviderIcon(const std::string& providerName, int cx, int cy, int radius);
    int getTextWidth(TTF_Font* font, const std::string& text) const;
    std::string ellipsizeText(TTF_Font* font, const std::string& text, int maxWidth) const;
    void drawText(TTF_Font* font, const std::string& text, int x, int y, SDL_Color color, bool center = false, int maxWidth = 0, bool rightAlign = false, bool dropShadow = false, bool vCenter = false);
    void drawWrappedText(TTF_Font* font, const std::string& text, const SDL_Rect& area, const std::string& vPos, const std::string& hAlign, SDL_Color color, bool dropShadow = false);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* wordmarkFont_ = nullptr;
    TTF_Font* fontTiny_ = nullptr;   // 13px (timestamps, badges)
    TTF_Font* fontSmall_ = nullptr;  // 15px (sublabels, artist/album, header)
    TTF_Font* fontNormal_ = nullptr; // 18px (button labels, track titles)
    TTF_Font* fontMedium_ = nullptr; // 22px (prominent titles, values)
    TTF_Font* fontLarge_ = nullptr;  // 28px (clocks, large readouts, modal headers)
    TTF_Font* fontXl_ = nullptr;     // 34px (weather temp, medium clock)
    TTF_Font* fontXxl_ = nullptr;    // 44px (hero digital clock)

    int focusRow_ = 0;
    int focusCol_ = 0;
    bool showFocusHighlight_ = false;

    // Floating Overlay Rects (when Status Bar is disabled)
    SDL_Rect rectFloatingBack_ = {16, 8, 96, 36};
    SDL_Rect rectFloatingSettings_ = {1164, 8, 100, 36};

    // Status Bar Controls (y = 0..54)
    SDL_Rect rectBarFolderPrev_ = {468, 9, 36, 36};
    SDL_Rect rectBarFolderPill_ = {512, 9, 256, 36};
    SDL_Rect rectBarFolderNext_ = {776, 9, 36, 36};
    SDL_Rect rectBarConn_ = {824, 9, 164, 36};
    SDL_Rect rectBarBattery_ = {998, 9, 156, 36};
    SDL_Rect rectBarSettings_ = {1164, 9, 100, 36};

    // Update Modal
    SDL_Rect rectUpdateModalDismiss_ = {530, 440, 220, 48};

    SettingsUI settingsUI_;
};
