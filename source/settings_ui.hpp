#pragma once
#include <string>
#include <vector>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "config.hpp"
#include "ws_client.hpp"
#include "deck_state.hpp"

enum class SettingsTab : int {
    Connection = 0,
    Appearance = 1,
    PagesAndTouch = 2,
    PowerAndDisplay = 3,
    Update = 4,
    About = 5,
    COUNT = 6
};

enum class ActiveField {
    IP,
    Port,
    PairingCode
};

enum class SettingsEvent {
    None = 0,
    ConfigChanged,
    GridSettingsChanged,
    ConnectRequested,
    ResetSessionRequested,
    AutoDiscoverRequested,
    ReloadDeckRequested,
    FolderChanged,
    ProfileChanged,
    CheckUpdateRequested
};

class SettingsUI {
public:
    SettingsUI();
    ~SettingsUI();

    void init(const AppConfig& config);
    void releaseTextures();
    void syncFromConfig(const AppConfig& config);
    void open();
    void close();
    bool isOpen() const { return isOpen_; }

    void render(SDL_Renderer* renderer,
                TTF_Font* fontTiny,
                TTF_Font* fontSmall,
                TTF_Font* fontNormal,
                TTF_Font* fontMedium,
                TTF_Font* fontLarge,
                TTF_Font* wordmarkFont,
                const DeckState& deckState,
                WsState wsState,
                int pingMs,
                const std::string& statusMsg,
                bool isDiscovering,
                const std::string& discoveryStatus,
                int batteryPct,
                bool isCharging);

    SettingsEvent handleTouch(int screenX, int screenY, AppConfig& ioConfig, DeckState& ioDeck, SDL_Renderer* renderer);
    SettingsEvent handleGamepad(int button, AppConfig& ioConfig, DeckState& ioDeck, SDL_Renderer* renderer);

    std::string getHost() const { return config_.host; }
    int getPort() const { return config_.port; }
    std::string getPairingCode() const { return config_.pairing_code; }

    void setUpdateStatus(const std::string& msg) { updateStatusMsg_ = msg; }

private:
    struct RowControlRects {
        int rowIndex = 0;
        SDL_Rect cardRect = {0, 0, 0, 0};
        SDL_Rect btnMinus = {0, 0, 0, 0};
        SDL_Rect btnPlus = {0, 0, 0, 0};
        SDL_Rect btnAction = {0, 0, 0, 0};
    };

    SettingsEvent adjustRow(int tab, int row, int dir, bool isClick, AppConfig& ioConfig, DeckState& ioDeck, SDL_Renderer* renderer);
    int getRowCountForTab(int tab) const;
    bool tryOpenNativeSwkbd(const char* headerText, std::string& ioText, int maxLen, bool numericOnly);

    void drawCard(SDL_Renderer* r, const SDL_Rect& rect, bool focused, bool isLight, bool isOled = false);
    void drawPillButton(SDL_Renderer* r, TTF_Font* font, const std::string& label, const SDL_Rect& rect, SDL_Color bg, SDL_Color fg, bool highlight = false);
    void drawStepperControl(SDL_Renderer* r, TTF_Font* fontNormal, TTF_Font* fontSmall, TTF_Font* fontMedium, const SDL_Rect& cardRect, const std::string& valueText, SDL_Rect& outMinus, SDL_Rect& outPlus, bool isLight, bool isOled = false, SDL_Color valueColor = {0, 0, 0, 0});
    void drawToggleControl(SDL_Renderer* r, TTF_Font* fontNormal, const SDL_Rect& cardRect, bool enabled, const char* onText, const char* offText, SDL_Rect& outBtn);
    void drawText(SDL_Renderer* r, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color, bool center = false, int maxWidth = 0, bool rightAlign = false);

    bool isOpen_ = false;
    int activeTab_ = 0;
    int focusedRow_ = 0;
    ActiveField activeField_ = ActiveField::PairingCode;
    AppConfig config_;
    std::string updateStatusMsg_ = "Sürüm kontrolü bekleniyor.";

    SDL_Rect rectBtnClose_ = {1110, 14, 154, 40};
    SDL_Rect tabRects_[6];
    std::vector<RowControlRects> currentRowRects_;

    SDL_Texture* qrTex_ = nullptr;
    bool qrAttempted_ = false;
};
