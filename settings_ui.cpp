#include "settings_ui.hpp"
#include "ui_renderer.hpp"
#include "updater.hpp"
#include <algorithm>
#include <iostream>
#include <SDL2/SDL_image.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

SettingsUI::SettingsUI() {}

SettingsUI::~SettingsUI() {
    releaseTextures();
}

void SettingsUI::releaseTextures() {
    if (qrTex_) {
        SDL_DestroyTexture(qrTex_);
        qrTex_ = nullptr;
    }
}

void SettingsUI::init(const AppConfig& config) {
    config_ = config;

    // Sidebar tab buttons (6 tabs: Baglanti, Gorunum, Sayfa, Guc, Guncelleme, Hakkinda)
    // Sidebar area: x=16, y=82..650. 6 tabs of height 58px, gap 8px.
    for (int i = 0; i < 6; ++i) {
        tabRects_[i] = {16, 82 + i * 66, 258, 58};
    }
}

void SettingsUI::syncFromConfig(const AppConfig& config) {
    config_ = config;
}

void SettingsUI::open() {
    isOpen_ = true;
    focusedRow_ = 0;
}

void SettingsUI::close() {
    isOpen_ = false;
}

int SettingsUI::getRowCountForTab(int tab) const {
    switch (tab) {
        case 0: return 7; // Baglanti & Sunucu
        case 1: return 5; // Gorunum & Tema (Dil, Tema, Bos Hucreler, Ust Cubuk, Sifirla)
        case 2: return 7; // Sayfa & Dokunma (Aktif Profil + 6 Sayfa Ayari)
        case 3: return 5; // Guc, Pil & Ekran
        case 4: return 2; // Guncelleme
        case 5: return 1; // Hakkinda
        default: return 0;
    }
}

bool SettingsUI::tryOpenNativeSwkbd(const char* headerText, std::string& ioText, int maxLen, bool numericOnly) {
#ifdef __SWITCH__
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) return false;

    swkbdConfigMakePresetDefault(&kbd);
    if (numericOnly) {
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    }
    if (headerText) {
        swkbdConfigSetHeaderText(&kbd, headerText);
    }
    swkbdConfigSetStringLenMax(&kbd, maxLen);
    swkbdConfigSetInitialText(&kbd, ioText.c_str());

    char outBuf[128] = {0};
    rc = swkbdShow(&kbd, outBuf, sizeof(outBuf));
    swkbdClose(&kbd);

    if (R_SUCCEEDED(rc) && outBuf[0] != '\0') {
        ioText = outBuf;
        return true;
    }
    return false;
#else
    (void)headerText;
    (void)ioText;
    (void)maxLen;
    (void)numericOnly;
    return false;
#endif
}

void SettingsUI::drawText(SDL_Renderer* r, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color, bool center, int maxWidth, bool rightAlign) {
    if (!r || !font || text.empty()) return;
    int texW = 0, texH = 0;
    SDL_Texture* tex = UIRenderer::getCachedTextTexture(r, font, text, texW, texH);
    if (!tex || texW <= 0 || texH <= 0) return;

    int drawW = texW;
    int drawH = texH;
    SDL_Rect src = {0, 0, drawW, drawH};
    if (maxWidth > 0 && drawW > maxWidth) {
        drawW = maxWidth;
        src.w = maxWidth;
    }
    int drawX = x;
    int drawY = y;
    if (center) {
        drawX = x - drawW / 2;
        drawY = y - drawH / 2;
    } else if (rightAlign) {
        drawX = x - drawW;
    }
    SDL_SetTextureColorMod(tex, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(tex, color.a);
    SDL_Rect dst = {drawX, drawY, drawW, drawH};
    SDL_RenderCopy(r, tex, &src, &dst);
}

void SettingsUI::drawCard(SDL_Renderer* r, const SDL_Rect& rect, bool focused, bool isLight, bool isOled) {
    SDL_Color bg;
    if (isOled) {
        bg = focused ? SDL_Color{22, 28, 40, 255} : SDL_Color{14, 16, 22, 255};
    } else if (isLight) {
        bg = focused ? SDL_Color{238, 244, 255, 255} : SDL_Color{255, 255, 255, 255};
    } else {
        bg = focused ? SDL_Color{32, 39, 54, 255} : SDL_Color{22, 26, 36, 255};
    }
    UIRenderer::drawRoundedBox(r, rect, 14, bg);
    if (focused) {
        UIRenderer::drawRoundedBorder(r, rect, 14, 2, {59, 130, 246, 255});
    } else {
        SDL_Color brd = isOled ? SDL_Color{28, 33, 44, 255}
                      : (isLight ? SDL_Color{215, 220, 230, 255} : SDL_Color{38, 45, 62, 255});
        UIRenderer::drawRoundedBorder(r, rect, 14, 1, brd);
    }
}

void SettingsUI::drawPillButton(SDL_Renderer* r, TTF_Font* font, const std::string& label, const SDL_Rect& rect, SDL_Color bg, SDL_Color fg, bool highlight) {
    UIRenderer::drawRoundedBox(r, rect, 12, bg);
    if (highlight) {
        UIRenderer::drawRoundedBorder(r, rect, 12, 2, {96, 165, 250, 255});
    }
    drawText(r, font, label, rect.x + rect.w / 2, rect.y + rect.h / 2, fg, true, rect.w - 12);
}

void SettingsUI::drawStepperControl(SDL_Renderer* r, TTF_Font* fontNormal, TTF_Font* fontSmall, TTF_Font* fontMedium, const SDL_Rect& cardRect, const std::string& valueText, SDL_Rect& outMinus, SDL_Rect& outPlus, bool isLight, bool isOled, SDL_Color valueColor) {
    // Touch-friendly 54x50 buttons with generous 236px value box between them
    int btnW = 54;
    int btnH = 50;
    int cy = cardRect.y + cardRect.h / 2;
    outPlus = {cardRect.x + cardRect.w - 16 - btnW, cy - btnH / 2, btnW, btnH};
    outMinus = {outPlus.x - 236 - btnW, cy - btnH / 2, btnW, btnH};

    SDL_Color btnBg = isOled ? SDL_Color{26, 32, 44, 255}
                     : (isLight ? SDL_Color{224, 231, 243, 255} : SDL_Color{36, 44, 62, 255});
    SDL_Color btnFg = isLight ? SDL_Color{15, 23, 42, 255} : SDL_Color{248, 250, 252, 255};
    drawPillButton(r, fontMedium ? fontMedium : fontNormal, "-", outMinus, btnBg, btnFg);
    drawPillButton(r, fontMedium ? fontMedium : fontNormal, "+", outPlus, btnBg, btnFg);

    SDL_Color valCol = (valueColor.a > 0) ? valueColor : (isLight ? SDL_Color{15, 23, 42, 255} : SDL_Color{248, 250, 252, 255});
    int valCenterX = (outMinus.x + outMinus.w + outPlus.x) / 2;
    int maxValW = (outPlus.x - 6) - (outMinus.x + outMinus.w + 6);
    TTF_Font* valFont = fontNormal;
    if (fontNormal && fontSmall && !valueText.empty()) {
        int tw = 0, th = 0;
        if (TTF_SizeUTF8(fontNormal, valueText.c_str(), &tw, &th) == 0 && tw > (maxValW - 10)) {
            valFont = fontSmall;
        }
    }
    drawText(r, valFont, valueText, valCenterX, cy, valCol, true, maxValW);
}

void SettingsUI::drawToggleControl(SDL_Renderer* r, TTF_Font* fontNormal, const SDL_Rect& cardRect, bool enabled, const char* onText, const char* offText, SDL_Rect& outBtn) {
    int h = 48;
    int w = 186;
    outBtn = {cardRect.x + cardRect.w - 16 - w, cardRect.y + (cardRect.h - h) / 2, w, h};
    SDL_Color bg = enabled ? SDL_Color{22, 163, 74, 255} : SDL_Color{64, 72, 88, 255};
    drawPillButton(r, fontNormal, enabled ? onText : offText, outBtn, bg, {255, 255, 255, 255});
}

void SettingsUI::render(SDL_Renderer* renderer,
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
                        bool isCharging)
{
    if (!isOpen_) return;

    bool isLight = (config_.theme == 1);
    bool isOled = (config_.theme == 2);

    SDL_Color pageBg = isOled ? SDL_Color{0, 0, 0, 255}
                     : (isLight ? SDL_Color{241, 245, 249, 255} : SDL_Color{13, 16, 23, 255});
    SDL_Color headerBg = isOled ? SDL_Color{13, 15, 20, 255}
                       : (isLight ? SDL_Color{255, 255, 255, 255} : SDL_Color{18, 22, 32, 255});
    SDL_Color sidebarBg = isOled ? SDL_Color{10, 11, 15, 255}
                        : (isLight ? SDL_Color{248, 250, 252, 255} : SDL_Color{16, 20, 29, 255});
    SDL_Color textPrimary = isLight ? SDL_Color{15, 23, 42, 255} : SDL_Color{248, 250, 252, 255};
    SDL_Color textMuted = isLight ? SDL_Color{100, 116, 139, 255} : SDL_Color{148, 163, 184, 255};

    // 1. Clear Screen
    SDL_SetRenderDrawColor(renderer, pageBg.r, pageBg.g, pageBg.b, 255);
    SDL_RenderClear(renderer);

    // 2. Top Header Bar (0, 0, 1280, 68)
    SDL_Rect headerRect = {0, 0, 1280, 68};
    SDL_SetRenderDrawColor(renderer, headerBg.r, headerBg.g, headerBg.b, 255);
    SDL_RenderFillRect(renderer, &headerRect);
    SDL_SetRenderDrawColor(renderer, isOled ? 28 : (isLight ? 220 : 36), isOled ? 33 : (isLight ? 226 : 42), isOled ? 44 : (isLight ? 235 : 56), 255);
    SDL_RenderDrawLine(renderer, 0, 68, 1280, 68);

    bool isEn = (config_.language == 1);

    UIRenderer::drawWordmark(renderer, wordmarkFont, fontSmall, 24, 25, textPrimary, isLight);
    drawText(renderer, fontSmall ? fontSmall : fontTiny, isEn ? "Settings & Control Panel" : "Ayarlar & Kontrol Paneli", 24, 42, textMuted);

    // Connection Pill & Battery Pill placed directly to the left of the Close button with small 12px gaps
    SDL_Rect connPill = {768, 16, 164, 36};
    SDL_Rect batPill  = {942, 16, 156, 36};
    UIRenderer::drawConnectionPill(renderer, fontSmall, fontTiny, connPill, wsState, pingMs, isLight, isOled, config_.language);
    UIRenderer::drawBatteryPill(renderer, fontSmall, batPill, batteryPct, isCharging, config_.low_battery_threshold, isLight, isOled, config_.language);

    // Close Button ("Deck'e Dön [B]")
    drawPillButton(renderer, fontSmall, isEn ? "Return to Deck [B]" : "Deck'e Dön [B]", rectBtnClose_, {37, 99, 235, 255}, {255, 255, 255, 255});

    // 3. Left Navigation Sidebar (0, 68, 290, 652)
    SDL_Rect sideRect = {0, 68, 290, 652};
    SDL_SetRenderDrawColor(renderer, sidebarBg.r, sidebarBg.g, sidebarBg.b, 255);
    SDL_RenderFillRect(renderer, &sideRect);
    SDL_SetRenderDrawColor(renderer, isOled ? 28 : (isLight ? 220 : 36), isOled ? 33 : (isLight ? 226 : 42), isOled ? 44 : (isLight ? 235 : 56), 255);
    SDL_RenderDrawLine(renderer, 290, 68, 290, 720);

    const char* tabTitles[6] = {
        isEn ? "Connection & Server" : "Bağlantı & Sunucu",
        isEn ? "Appearance & Theme" : "Görünüm & Tema",
        isEn ? "Pages & Touch" : "Sayfa & Dokunma",
        isEn ? "Power, Battery & Display" : "Güç, Pil & Ekran",
        isEn ? "Update" : "Güncelleme",
        isEn ? "About" : "Hakkında"
    };
    const char* tabSubs[6] = {
        isEn ? "IP, Port, Auto Discovery" : "IP, Port, Otomatik Bul",
        isEn ? "Theme, Empty Cells, Style" : "Tema, Boş Kutular, Stil",
        isEn ? "Folders, Press Durations" : "Klasörler, Basma Süreleri",
        isEn ? "Brightness, Sleep, Battery" : "Parlaklık, Uyku, Pil",
        isEn ? "Check Version & Download" : "Sürüm Kontrolü & İndirme",
        "SwitchDeck " SWITCHDECK_VERSION_TAG " (SertAy)"
    };

    for (int i = 0; i < 6; ++i) {
        bool selected = (activeTab_ == i);
        SDL_Color tBg = selected ? SDL_Color{37, 99, 235, 255}
                                 : (isOled ? SDL_Color{16, 20, 28, 255} : (isLight ? SDL_Color{235, 240, 247, 255} : SDL_Color{24, 29, 42, 255}));
        UIRenderer::drawRoundedBox(renderer, tabRects_[i], 12, tBg);
        SDL_Color titleCol = selected ? SDL_Color{255, 255, 255, 255} : textPrimary;
        SDL_Color subCol = selected ? SDL_Color{219, 234, 254, 255} : textMuted;
        drawText(renderer, fontNormal, tabTitles[i], tabRects_[i].x + 16, tabRects_[i].y + 9, titleCol);
        drawText(renderer, fontTiny ? fontTiny : fontSmall, tabSubs[i], tabRects_[i].x + 16, tabRects_[i].y + 32, subCol);
    }

    drawText(renderer, fontSmall, isEn ? "L / R : Switch Tab" : "L / R : Sekme Değiştir", 22, 592, textMuted);
    drawText(renderer, fontSmall, isEn ? "D-Pad / Touch : Adjust" : "D-Pad / Dokun : Değiştir", 22, 622, textMuted);
    drawText(renderer, fontSmall, isEn ? "B : Return to Deck" : "B : Deck'e Geri Dön", 22, 652, textMuted);
    drawText(renderer, fontSmall, isEn ? "+ : Exit App" : "+ : Uygulamadan Çık", 22, 682, textMuted);

    // 4. Right Content Area (290..1280, 68..720)
    currentRowRects_.clear();
    bool hasToken = !config_.access_token.empty() || !config_.refresh_cookie.empty();

    if (activeTab_ == static_cast<int>(SettingsTab::Connection)) {
        // Tab 0: Connection & Server Discovery
        int startY = 80;
        int rowH = 74;
        int gap = 10;

        struct ConnRowInfo {
            const char* title;
            std::string sub;
        };
        std::string discSub;
        if (isEn) {
            discSub = isDiscovering ? discoveryStatus : (discoveryStatus.empty() ? "Automatically finds Macro Deck 3 server on local Wi-Fi" : discoveryStatus);
        } else {
            discSub = isDiscovering ? discoveryStatus : (discoveryStatus.empty() ? "Yerel Wi-Fi ağındaki Macro Deck 3 sunucusunu otomatik bulur" : discoveryStatus);
        }
        ConnRowInfo rows[7] = {
            {isEn ? "Computer IP Address" : "Bilgisayar IP Adresi", isEn ? "Local IP of computer running Macro Deck 3 (e.g. 192.168.1.43)" : "Macro Deck 3 çalışan bilgisayarın yerel IP adresi (Örn: 192.168.1.43)"},
            {isEn ? "Server Port" : "Sunucu Portu", isEn ? "Macro Deck 3 default: 8193 (change with + / - or tap)" : "Macro Deck 3 varsayılan: 8193 (+ / - ile değiştirin veya dokunun)"},
            {isEn ? "6-Digit Pairing Code" : "6 Haneli Eşleme Kodu", hasToken ? (isEn ? "Saved session ready! Enter code for new pairing" : "Kayıtlı oturum hazır! Yeni eşleme için kod girin") : (isEn ? "Code from PC -> Macro Deck -> Devices -> Add Device" : "PC -> Macro Deck -> Cihazlar -> Cihaz Ekle ekranındaki kod")},
            {isEn ? "Find Macro Deck on the Network" : "Ağda Macro Deck’i Bul", discSub},
            {isEn ? "Connection Status" : "Bağlantı Durumu", statusMsg},
            {isEn ? "Auto Reconnect" : "Otomatik Yeniden Bağlan", isEn ? "Try again automatically when the connection drops" : "Bağlantı koparsa kendiliğinden yeniden dener"},
            {isEn ? "Forget Pairing" : "Eşleştirmeyi Unut", isEn ? "A new code is required to connect again" : "Tekrar bağlanmak için bilgisayardan yeni kod al"}
        };

        for (int r = 0; r < 7; ++r) {
            RowControlRects rc;
            rc.rowIndex = r;
            rc.cardRect = {316, startY + r * (rowH + gap), 936, rowH};
            bool focused = (focusedRow_ == r);
            drawCard(renderer, rc.cardRect, focused, isLight, isOled);

            drawText(renderer, fontNormal, rows[r].title, rc.cardRect.x + 18, rc.cardRect.y + 13, textPrimary);
            drawText(renderer, fontSmall, rows[r].sub, rc.cardRect.x + 18, rc.cardRect.y + 42, textMuted, false, 530);

            if (r == 0) {
                // IP Field
                bool active = (activeField_ == ActiveField::IP);
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 280, rc.cardRect.y + 14, 264, 46};
                SDL_Color fBg = active ? SDL_Color{30, 58, 138, 255} : (isLight ? SDL_Color{226, 232, 240, 255} : (isOled ? SDL_Color{22, 26, 36, 255} : SDL_Color{34, 41, 56, 255}));
                drawPillButton(renderer, fontNormal, config_.host, rc.btnAction, fBg, active ? SDL_Color{255, 255, 255, 255} : textPrimary, active);
            } else if (r == 1) {
                // Port Field + Stepper
                bool active = (activeField_ == ActiveField::Port);
                std::string disp = std::to_string(config_.port);
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, disp, rc.btnMinus, rc.btnPlus, isLight, isOled,
                                   active ? SDL_Color{96, 165, 250, 255} : SDL_Color{0, 0, 0, 0});
                rc.btnAction = {rc.btnMinus.x + rc.btnMinus.w + 6, rc.btnMinus.y, rc.btnPlus.x - (rc.btnMinus.x + rc.btnMinus.w) - 12, rc.btnMinus.h};
            } else if (r == 2) {
                // Pairing Code Field
                bool active = (activeField_ == ActiveField::PairingCode);
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 280, rc.cardRect.y + 14, 264, 46};
                SDL_Color fBg = active ? SDL_Color{30, 58, 138, 255} : (isLight ? SDL_Color{226, 232, 240, 255} : (isOled ? SDL_Color{22, 26, 36, 255} : SDL_Color{34, 41, 56, 255}));
                std::string disp = config_.pairing_code.empty()
                    ? (hasToken ? (isEn ? "SESSION SAVED" : "OTURUM KAYITLI") : (isEn ? "ENTER CODE" : "KOD GİRİN"))
                    : config_.pairing_code;
                SDL_Color fg = (config_.pairing_code.empty() && hasToken && !active) ? SDL_Color{74, 222, 128, 255}
                             : (active ? SDL_Color{255, 255, 255, 255} : textPrimary);
                drawPillButton(renderer, fontNormal, disp, rc.btnAction, fBg, fg, active);
            } else if (r == 3) {
                // Auto-Discover PC Button
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 246, rc.cardRect.y + 14, 230, 46};
                SDL_Color bCol = isDiscovering ? SDL_Color{180, 83, 9, 255} : SDL_Color{37, 99, 235, 255};
                std::string discLabel = isDiscovering ? (isEn ? "Searching..." : "Aranıyor…") : (isEn ? "Search Network" : "Ağda Ara");
                drawPillButton(renderer, fontNormal, discLabel, rc.btnAction, bCol, {255, 255, 255, 255});
            } else if (r == 4) {
                // Connect Now Button
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 246, rc.cardRect.y + 14, 230, 46};
                drawPillButton(renderer, fontNormal, isEn ? "Connect Now" : "Şimdi Bağlan", rc.btnAction, {22, 163, 74, 255}, {255, 255, 255, 255});
            } else if (r == 5) {
                // Auto-reconnect toggle
                drawToggleControl(renderer, fontNormal, rc.cardRect, config_.auto_reconnect, isEn ? "ON" : "AÇIK", isEn ? "OFF" : "KAPALI", rc.btnAction);
            } else if (r == 6) {
                // Reset Auth button
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 246, rc.cardRect.y + 14, 230, 46};
                drawPillButton(renderer, fontNormal, isEn ? "Forget Pairing" : "Eşleştirmeyi Unut", rc.btnAction, {153, 27, 27, 255}, {255, 255, 255, 255});
            }

            currentRowRects_.push_back(rc);
        }

    } else if (activeTab_ == static_cast<int>(SettingsTab::Appearance)) {
        // Tab 1: Appearance & Display Styling
        int startY = 88;
        int rowH = 84;
        int gap = 14;

        const char* titles[5] = {
            isEn ? "Interface Language" : "Uygulama Dili",
            isEn ? "Application Theme" : "Uygulama Teması",
            isEn ? "Empty Grid Cells" : "Boş Izgara Hücreleri",
            isEn ? "Top Status Bar" : "Üst Durum Çubuğu",
            isEn ? "Reset to Defaults" : "Varsayılana Döndür"
        };
        const char* subs[5] = {
            isEn ? "Language of SwitchDeck menus and buttons" : "SwitchDeck menülerinin ve düğmelerinin dili",
            isEn ? "Dark (Macro Deck 3), Light, or OLED pure black deep contrast" : "Koyu (Macro Deck 3), Açık veya OLED derin siyah teması",
            isEn ? "Show or hide unassigned empty grid button frames" : "Atanmamış boş ızgara kutularını ekranda göster veya gizle",
            isEn ? "Show top status bar or switch to full-screen deck mode" : "Üst bilgi çubuğunu açık tut veya tam ekran deck görünümüne geç",
            isEn ? "Reset theme and interface preferences to default values" : "Tema ve arayüz görünüm ayarlarını varsayılana sıfırlar"
        };

        for (int r = 0; r < 5; ++r) {
            RowControlRects rc;
            rc.rowIndex = r;
            rc.cardRect = {316, startY + r * (rowH + gap), 936, rowH};
            drawCard(renderer, rc.cardRect, (focusedRow_ == r), isLight, isOled);

            drawText(renderer, fontNormal, titles[r], rc.cardRect.x + 18, rc.cardRect.y + 14, textPrimary);
            drawText(renderer, fontSmall, subs[r], rc.cardRect.x + 18, rc.cardRect.y + 46, textMuted, false, 540);

            if (r == 0) {
                std::string langStr = isEn ? "English (EN)" : "Türkçe (TR)";
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, langStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 1) {
                std::string valStr;
                if (config_.theme == 0) valStr = "Macro Deck";
                else if (config_.theme == 1) valStr = isEn ? "Light" : "Açık (Light)";
                else valStr = isEn ? "OLED Pure Black" : "OLED Tam Siyah";
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, valStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 2) {
                std::string valStr;
                if (config_.show_empty_cells < 0) valStr = isEn ? "Follow Macro Deck" : "Macro Deck’i İzle";
                else if (config_.show_empty_cells == 1) valStr = isEn ? "Always Show" : "Her Zaman Göster";
                else valStr = isEn ? "Hide" : "Gizle";
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, valStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 3) {
                drawToggleControl(renderer, fontNormal, rc.cardRect, config_.show_status_bar, isEn ? "SHOW" : "GÖSTER", isEn ? "HIDE" : "GİZLE", rc.btnAction);
            } else if (r == 4) {
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 186 - 16, rc.cardRect.y + (rc.cardRect.h - 48) / 2, 186, 48};
                drawPillButton(renderer, fontNormal, isEn ? "Reset" : "Sıfırla", rc.btnAction, {220, 38, 38, 255}, {255, 255, 255, 255});
            }
            currentRowRects_.push_back(rc);
        }

    } else if (activeTab_ == static_cast<int>(SettingsTab::PagesAndTouch)) {
        // Tab 2: Pages, Folders & Touch Gestures
        int startY = 80;
        int rowH = 76;
        int gap = 10;

        std::string curProfName = deckState.getCurrentProfileName();
        if (curProfName.empty()) curProfName = "Default";

        auto folders = deckState.getActiveProfileFolders();
        int fCount = static_cast<int>(folders.size());
        int curIdx = deckState.getCurrentFolderIndex();
        std::string curFolderLabel = deckState.getCurrentFolderName();
        if (fCount > 0) {
            curFolderLabel += " (" + std::to_string(curIdx + 1) + "/" + std::to_string(fCount) + ")";
        }

        std::string startupLabel = isEn ? "Default (Home)" : "Varsayılan (Home)";
        if (!config_.startup_folder_id.empty()) {
            startupLabel = config_.startup_folder_id;
            for (const auto& f : folders) {
                if (f.id == config_.startup_folder_id) {
                    startupLabel = f.name;
                    break;
                }
            }
        }

        const char* titles[7] = {
            isEn ? "Active Profile" : "Aktif Profil",
            isEn ? "Active Page / Folder" : "Aktif Sayfa / Klasör",
            isEn ? "Startup Page / Folder" : "Başlangıç Sayfası / Klasörü",
            isEn ? "Remember Last Page" : "Son Kalınan Sayfayı Hatırla",
            isEn ? "Long Press Duration" : "Uzun Basma Süresi",
            isEn ? "Double Tap Duration" : "Çift Dokunma Süresi",
            isEn ? "Reload Profile & Folders" : "Profili ve Klasörleri Yenile"
        };
        const char* subs[7] = {
            isEn ? "Active Macro Deck profile on PC" : "PC'de kullanılan aktif Macro Deck profili",
            isEn ? "Switch currently displayed folder in active profile" : "Aktif profilde görüntülenen Macro Deck sayfasını / klasörünü değiştirir",
            isEn ? "Opens at startup if Remember Last Page is off" : "Son Sayfayı Hatırla kapalıysa açılışta bu sayfa kullanılır",
            isEn ? "Open the last used page at startup" : "Açılışta en son kullandığın sayfaya dön",
            isEn ? "Hold duration to trigger 'Long Press' action" : "Parmağınızı basılı tuttuğunuzda 'Uzun Basma' tetiklenme süresi",
            isEn ? "Maximum delay between taps for 'Double Tap' action" : "İki kez hızlıca dokunduğunuzda 'Çift Dokunma' olarak algılanma süresi",
            isEn ? "Re-fetches all profiles, folders and buttons from PC" : "PC'deki tüm profil, sayfa ve buton değişikliklerini anında tekrar çeker"
        };

        for (int r = 0; r < 7; ++r) {
            RowControlRects rc;
            rc.rowIndex = r;
            rc.cardRect = {316, startY + r * (rowH + gap), 936, rowH};
            drawCard(renderer, rc.cardRect, (focusedRow_ == r), isLight, isOled);

            drawText(renderer, fontNormal, titles[r], rc.cardRect.x + 18, rc.cardRect.y + 11, textPrimary);
            drawText(renderer, fontSmall, subs[r], rc.cardRect.x + 18, rc.cardRect.y + 40, textMuted, false, 560);

            if (r == 0) {
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, curProfName, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 1) {
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, curFolderLabel, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 2) {
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, startupLabel, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 3) {
                drawToggleControl(renderer, fontNormal, rc.cardRect, config_.remember_last_folder, isEn ? "ON" : "AÇIK", isEn ? "OFF" : "KAPALI", rc.btnAction);
            } else if (r == 4) {
                std::string lpStr = std::to_string(config_.long_press_ms) + " ms";
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, lpStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 5) {
                std::string dtStr = std::to_string(config_.double_tap_ms) + " ms";
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, dtStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 6) {
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 220, rc.cardRect.y + 14, 200, 48};
                drawPillButton(renderer, fontNormal, isEn ? "Sync Now" : "Yenile (Sync)", rc.btnAction, {37, 99, 235, 255}, {255, 255, 255, 255});
            }
            currentRowRects_.push_back(rc);
        }

    } else if (activeTab_ == static_cast<int>(SettingsTab::PowerAndDisplay)) {
        // Tab 3: Power, Battery & Display
        int startY = 88;
        int rowH = 84;
        int gap = 14;

        const char* titles[5] = {
            isEn ? "Sleep Mode" : "Uyku Modu",
            isEn ? "Screen Brightness" : "Ekran Parlaklığı",
            isEn ? "Idle Timeout" : "Boşta Bekleme Süresi",
            isEn ? "Screen When Idle" : "Boşta Ekran Görünümü",
            isEn ? "Low Battery Warning" : "Düşük Pil Uyarısı"
        };
        const char* subs[5] = {
            isEn ? "Apply the selected idle screen mode when the wait ends" : "Açıksa bekleme süresi dolunca seçilen ekran modu devreye girer",
            isEn ? "Adjusts your Switch screen brightness" : "Switch ekranının parlaklığını ayarlar",
            isEn ? "Wait time after your last touch or button press" : "Son dokunuş veya tuş basışından sonraki bekleme süresi",
            isEn ? "Sleep the console, show black for OLED, or dim the screen" : "Switch’i uyut, OLED için siyah göster veya ekranı karart",
            isEn ? "Displays red alert in status bar when battery falls below" : "Pil seviyesi bu değerin altına indiğinde üst çubukta kırmızı ikaz belirir"
        };

        for (int r = 0; r < 5; ++r) {
            RowControlRects rc;
            rc.rowIndex = r;
            rc.cardRect = {316, startY + r * (rowH + gap), 936, rowH};
            drawCard(renderer, rc.cardRect, (focusedRow_ == r), isLight, isOled);

            drawText(renderer, fontNormal, titles[r], rc.cardRect.x + 18, rc.cardRect.y + 14, textPrimary);
            drawText(renderer, fontSmall, subs[r], rc.cardRect.x + 18, rc.cardRect.y + 46, textMuted, false, 510);

            if (r == 0) {
                drawToggleControl(renderer, fontNormal, rc.cardRect, config_.idle_enabled, isEn ? "ON" : "AÇIK", isEn ? "OFF" : "KAPALI", rc.btnAction);
            } else if (r == 1) {
                std::string brStr = "%" + std::to_string(config_.brightness_pct);
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, brStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 2) {
                std::string toStr;
                if (config_.idle_timeout_sec % 60 != 0) toStr = std::to_string(config_.idle_timeout_sec) + (isEn ? " seconds" : " saniye");
                else toStr = std::to_string(config_.idle_timeout_sec / 60) + (isEn ? " minutes" : " dakika");
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, toStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 3) {
                const char* imStr = isEn ? "Dim" : "Karart";
                if (config_.idle_mode == IdleScreenMode::Sleep) imStr = isEn ? "Turn Screen Off" : "Ekranı Kapat";
                if (config_.idle_mode == IdleScreenMode::OledBlack) imStr = isEn ? "Black Screen (OLED)" : "Siyah Ekran (OLED)";
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, imStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            } else if (r == 4) {
                std::string lbStr = (config_.low_battery_threshold <= 0)
                    ? (isEn ? "OFF" : "KAPALI")
                    : ("%" + std::to_string(config_.low_battery_threshold));
                drawStepperControl(renderer, fontNormal, fontSmall, fontMedium, rc.cardRect, lbStr, rc.btnMinus, rc.btnPlus, isLight, isOled);
            }
            currentRowRects_.push_back(rc);
        }

    } else if (activeTab_ == static_cast<int>(SettingsTab::Update)) {
        // Tab 4: Update (Güncelleme)
        int startY = 110;
        int rowH = 104;
        int gap = 20;

        struct UpdRowInfo {
            const char* title;
            std::string sub;
        };
        UpdRowInfo rows[2] = {
            {isEn ? "Installed Version" : "Yüklü Sürüm", "SwitchDeck " SWITCHDECK_VERSION_TAG},
            {isEn ? "Check Online Update" : "Çevrimiçi Güncelleme Kontrolü", updateStatusMsg_}
        };

        for (int r = 0; r < 2; ++r) {
            RowControlRects rc;
            rc.rowIndex = r;
            rc.cardRect = {316, startY + r * (rowH + gap), 936, rowH};
            drawCard(renderer, rc.cardRect, (focusedRow_ == r), isLight, isOled);

            drawText(renderer, fontNormal, rows[r].title, rc.cardRect.x + 24, rc.cardRect.y + 22, textPrimary);
            drawText(renderer, fontSmall, rows[r].sub, rc.cardRect.x + 24, rc.cardRect.y + 60, textMuted, false, 580);

            if (r == 0) {
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 200, rc.cardRect.y + 28, 176, 48};
                drawPillButton(renderer, fontNormal, SWITCHDECK_VERSION_TAG, rc.btnAction, {22, 163, 74, 255}, {255, 255, 255, 255});
            } else if (r == 1) {
                rc.btnAction = {rc.cardRect.x + rc.cardRect.w - 260, rc.cardRect.y + 28, 236, 48};
                drawPillButton(renderer, fontNormal, isEn ? "Check Updates" : "Güncellemeleri Denetle", rc.btnAction, {37, 99, 235, 255}, {255, 255, 255, 255});
            }
            currentRowRects_.push_back(rc);
        }

    } else if (activeTab_ == static_cast<int>(SettingsTab::About)) {
        // Tab 5: About (Hakkında) - Matching the exact visual layout requested
        RowControlRects rc;
        rc.rowIndex = 0;
        rc.cardRect = {316, 76, 936, 624};
        drawCard(renderer, rc.cardRect, (focusedRow_ == 0), isLight, isOled);

        int cx = rc.cardRect.x + rc.cardRect.w / 2;

        // 1. Top Title: "SertAy Tüm Linkler"
        drawText(renderer, fontLarge ? fontLarge : fontMedium, isEn ? "SertAy Links" : "SertAy Tüm Linkler", cx, rc.cardRect.y + 32, textPrimary, true);

        // 2. Poll for new QR downloaded in background from GitHub
        if (AppUpdater::pollNewQr()) {
            releaseTextures();
            qrAttempted_ = false;
        }

        // 3. Load QR Code texture if not loaded yet
        if (!qrTex_ && !qrAttempted_) {
            qrAttempted_ = true;
            const char* qrPaths[] = {
                "sdmc:/switch/SwitchDeck/contact_qr.png",
                "switch/SwitchDeck/contact_qr.png",
                "romfs:/contact_qr.png",
                "contact_qr.png"
            };
            for (const char* p : qrPaths) {
                SDL_Surface* s = IMG_Load(p);
                if (s) {
                    qrTex_ = SDL_CreateTextureFromSurface(renderer, s);
                    SDL_FreeSurface(s);
                    break;
                }
            }
        }

        // 4. Badge positioned cleanly between "SertAy Tüm Linkler" and the QR Code card
        int badgeW = 176;
        int badgeH = 28;
        int badgeY = rc.cardRect.y + 66;
        SDL_Rect badgeRect = { cx - badgeW / 2, badgeY, badgeW, badgeH };
        UIRenderer::drawRoundedBox(renderer, badgeRect, 10, {99, 102, 241, 255}); // Purple / indigo badge
        drawText(renderer, fontSmall ? fontSmall : fontNormal, isEn ? "SCAN WITH CAMERA" : "KAMERAYLA OKUT", cx, badgeY + badgeH / 2, {255, 255, 255, 255}, true);

        // 5. Center QR Code inside rounded white card below the badge
        int qrSize = 196;
        int qrFrameW = 230;
        int qrFrameH = 230;
        int qrFrameY = rc.cardRect.y + 112;
        SDL_Rect qrFrame = { cx - qrFrameW / 2, qrFrameY, qrFrameW, qrFrameH };
        UIRenderer::drawRoundedBox(renderer, qrFrame, 20, {255, 255, 255, 255});

        if (qrTex_) {
            SDL_Rect qrDst = { cx - qrSize / 2, qrFrameY + (qrFrameH - qrSize) / 2, qrSize, qrSize };
            SDL_RenderCopy(renderer, qrTex_, nullptr, &qrDst);
        } else {
            drawText(renderer, fontSmall, "QR Kodu Yükleniyor...", cx, qrFrameY + qrFrameH / 2, {100, 116, 139, 255}, true);
        }

        // 6. Left Cards: Instagram (top) and YouTube (bottom)
        int cardW = 196;
        int cardH = 50;
        int leftCardX = qrFrame.x - cardW - 32;

        // 6a. Instagram Card
        int instaY = qrFrameY + 36;
        SDL_Rect instaRect = { leftCardX, instaY, cardW, cardH };
        UIRenderer::drawRoundedBox(renderer, instaRect, 14, {24, 18, 32, 240});
        UIRenderer::drawRoundedBorder(renderer, instaRect, 14, 2, {225, 48, 108, 255}); // Magenta/pink
        int iconX = leftCardX + 16;
        int iconY = instaY + 13;
        SDL_Rect camRect = { iconX, iconY, 24, 24 };
        UIRenderer::drawRoundedBorder(renderer, camRect, 6, 2, {225, 48, 108, 255});
        UIRenderer::drawFilledCircleAA(renderer, iconX + 12, iconY + 12, 5, {0, 0, 0, 0}, {225, 48, 108, 255}, 2);
        UIRenderer::drawFilledCircleAA(renderer, iconX + 18, iconY + 6, 1, {225, 48, 108, 255});
        drawText(renderer, fontNormal ? fontNormal : fontMedium, "cilsertay", leftCardX + 50, instaY + 13, {255, 255, 255, 255});

        // 6b. YouTube Card
        int ytY = qrFrameY + 116;
        SDL_Rect ytRect = { leftCardX, ytY, cardW, cardH };
        UIRenderer::drawRoundedBox(renderer, ytRect, 14, {34, 18, 18, 240});
        UIRenderer::drawRoundedBorder(renderer, ytRect, 14, 2, {239, 68, 68, 255}); // Red
        int ytIconX = leftCardX + 16;
        int ytIconY = ytY + 16;
        SDL_Rect ytBadge = { ytIconX, ytIconY, 24, 18 };
        UIRenderer::drawRoundedBox(renderer, ytBadge, 4, {239, 68, 68, 255});
        int triX = ytIconX + 10;
        int triY = ytIconY + 9;
        for (int i = 0; i <= 6; ++i) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDrawLine(renderer, triX + i, triY - (6 - i) / 2, triX + i, triY + (6 - i) / 2);
        }
        drawText(renderer, fontNormal ? fontNormal : fontMedium, "SertAy", leftCardX + 50, ytY + 13, {255, 255, 255, 255});

        // 7. Right Card: TikTok (vertically centered)
        int rightCardX = qrFrame.x + qrFrame.w + 32;
        int tiktokY = qrFrameY + 76;
        SDL_Rect tiktokRect = { rightCardX, tiktokY, 204, cardH };
        UIRenderer::drawRoundedBox(renderer, tiktokRect, 14, {16, 28, 32, 240});
        UIRenderer::drawRoundedBorder(renderer, tiktokRect, 14, 2, {37, 244, 238, 255}); // Cyan / teal
        int ttX = rightCardX + 18;
        int ttY = tiktokY + 14;
        UIRenderer::drawFilledCircleAA(renderer, ttX + 6, ttY + 16, 4, {37, 244, 238, 255});
        SDL_SetRenderDrawColor(renderer, 37, 244, 238, 255);
        for (int w = 0; w < 3; ++w) {
            SDL_RenderDrawLine(renderer, ttX + 9 + w, ttY + 4, ttX + 9 + w, ttY + 16);
            SDL_RenderDrawLine(renderer, ttX + 9 + w, ttY + 4, ttX + 17 + w, ttY + 10);
        }
        UIRenderer::drawFilledCircleAA(renderer, ttX + 4, ttY + 16, 2, {254, 44, 85, 255});
        drawText(renderer, fontNormal ? fontNormal : fontMedium, "sertayintardis", rightCardX + 48, tiktokY + 13, {255, 255, 255, 255});

        // 8. Bottom Follow Caption
        const char* followCaption = isEn
            ? "FOLLOW ME to support and stay updated on new releases!"
            : "Desteklemek ve güncellemelerden haberdar olmak için BENİ TAKİP EDİN!";
        drawText(renderer, fontNormal ? fontNormal : fontMedium, followCaption, cx, rc.cardRect.y + 510, {248, 250, 252, 255}, true);

        currentRowRects_.push_back(rc);
    }
}

SettingsEvent SettingsUI::adjustRow(int tab, int row, int dir, bool isClick, AppConfig& ioConfig, DeckState& ioDeck, SDL_Renderer* renderer) {
    if (tab == static_cast<int>(SettingsTab::Connection)) {
        if (row == 0) {
            activeField_ = ActiveField::IP;
            if (isClick) {
                if (tryOpenNativeSwkbd("Bilgisayar IP Adresi (Örn: 192.168.1.43)", config_.host, 32, false)) {
                    ioConfig = config_;
                }
            }
        } else if (row == 1) {
            activeField_ = ActiveField::Port;
            if (dir != 0) {
                config_.port = std::clamp(config_.port + dir, 1, 65535);
                ioConfig = config_;
            } else if (isClick) {
                std::string pStr = std::to_string(config_.port);
                if (tryOpenNativeSwkbd("Macro Deck Sunucu Portu (Varsayılan 8193)", pStr, 5, true)) {
                    try {
                        config_.port = std::clamp(std::stoi(pStr), 1, 65535);
                        ioConfig = config_;
                    } catch (...) {}
                }
            }
        } else if (row == 2) {
            activeField_ = ActiveField::PairingCode;
            if (isClick) {
                if (tryOpenNativeSwkbd("6 Haneli Eşleme Kodu (Macro Deck -> Cihaz Ekle)", config_.pairing_code, 8, false)) {
                    ioConfig = config_;
                }
            }
        } else if (row == 3) {
            return SettingsEvent::AutoDiscoverRequested;
        } else if (row == 4) {
            ioConfig = config_;
            return SettingsEvent::ConnectRequested;
        } else if (row == 5) {
            config_.auto_reconnect = !config_.auto_reconnect;
            ioConfig = config_;
        } else if (row == 6) {
            activeField_ = ActiveField::PairingCode;
            return SettingsEvent::ResetSessionRequested;
        }

    } else if (tab == static_cast<int>(SettingsTab::Appearance)) {
        int step = (dir != 0) ? dir : 1;
        if (row == 0) {
            config_.language = (config_.language == 0) ? 1 : 0;
            ioConfig = config_;
        } else if (row == 1) {
            config_.theme = (config_.theme + step + 3) % 3;
            ioConfig = config_;
        } else if (row == 2) {
            // Empty Cells: Auto (-1) -> Show (1) -> Hide (0)
            if (config_.show_empty_cells < 0) config_.show_empty_cells = (step > 0) ? 1 : 0;
            else if (config_.show_empty_cells == 1) config_.show_empty_cells = (step > 0) ? 0 : -1;
            else config_.show_empty_cells = (step > 0) ? -1 : 1;
            ioConfig = config_;
        } else if (row == 3) {
            config_.show_status_bar = !config_.show_status_bar;
            ioConfig = config_;
        } else if (row == 4) {
            // Reset to Defaults: restore default theme and clear any legacy overrides
            config_.theme = 0;
            config_.button_scale_pct = 100;
            config_.spacing_override = -1;
            config_.radius_override = -1;
            config_.show_empty_cells = -1;
            config_.show_status_bar = true;
            config_.grid_rows_override = 0;
            config_.grid_cols_override = 0;
            ioConfig = config_;
            return SettingsEvent::GridSettingsChanged;
        }

    } else if (tab == static_cast<int>(SettingsTab::PagesAndTouch)) {
        int step = (dir != 0) ? dir : 1;
        if (row == 0) {
            // Profile switcher
            const auto& profiles = ioDeck.getProfiles();
            if (profiles.size() > 1) {
                int curPIdx = 0;
                for (size_t i = 0; i < profiles.size(); ++i) {
                    if (profiles[i].id == ioDeck.getCurrentProfileId()) {
                        curPIdx = static_cast<int>(i);
                        break;
                    }
                }
                int nextPIdx = (curPIdx + step + static_cast<int>(profiles.size())) % static_cast<int>(profiles.size());
                if (ioDeck.switchProfile(profiles[nextPIdx].id, renderer)) {
                    config_.startup_profile_id = ioDeck.getCurrentProfileId();
                    config_.last_folder_id = ioDeck.getCurrentFolderId();
                    ioConfig = config_;
                    return SettingsEvent::ProfileChanged;
                }
            }
            return SettingsEvent::None;
        } else if (row == 1) {
            bool moved = (step > 0) ? ioDeck.nextFolder(renderer) : ioDeck.prevFolder(renderer);
            if (moved) {
                if (config_.remember_last_folder) {
                    config_.last_folder_id = ioDeck.getCurrentFolderId();
                    ioConfig = config_;
                }
                return SettingsEvent::FolderChanged;
            }
            return SettingsEvent::None;
        } else if (row == 2) {
            auto folders = ioDeck.getActiveProfileFolders();
            if (!folders.empty()) {
                int cur = -1;
                for (size_t i = 0; i < folders.size(); ++i) {
                    if (folders[i].id == config_.startup_folder_id) {
                        cur = static_cast<int>(i);
                        break;
                    }
                }
                int total = static_cast<int>(folders.size()) + 1;
                int next = (cur + 1 + step + total) % total;
                config_.startup_folder_id = (next == 0) ? "" : folders[next - 1].id;
            } else {
                config_.startup_folder_id.clear();
            }
            ioConfig = config_;
        } else if (row == 3) {
            config_.remember_last_folder = !config_.remember_last_folder;
            if (config_.remember_last_folder) {
                config_.last_folder_id = ioDeck.getCurrentFolderId();
            }
            ioConfig = config_;
        } else if (row == 4) {
            config_.long_press_ms = std::clamp(config_.long_press_ms + step * 50, 250, 1500);
            ioConfig = config_;
        } else if (row == 5) {
            config_.double_tap_ms = std::clamp(config_.double_tap_ms + step * 20, 120, 700);
            ioConfig = config_;
        } else if (row == 6) {
            return SettingsEvent::ReloadDeckRequested;
        }

    } else if (tab == static_cast<int>(SettingsTab::PowerAndDisplay)) {
        int step = (dir != 0) ? dir : 1;
        if (row == 0) {
            config_.idle_enabled = !config_.idle_enabled;
            ioConfig = config_;
        } else if (row == 1) {
            // Brightness % (10..100)
            config_.brightness_pct = std::clamp(config_.brightness_pct + step * 5, 10, 100);
            ioConfig = config_;
#ifdef __SWITCH__
            if (R_SUCCEEDED(lblInitialize())) {
                float br = std::clamp(static_cast<float>(config_.brightness_pct) / 100.0f, 0.05f, 1.0f);
                lblSetCurrentBrightnessSetting(br);
                lblApplyCurrentBrightnessSettingToBacklight();
                lblExit();
            }
#endif
        } else if (row == 2) {
            static const int timeouts[] = {60, 180, 300, 600, 900, 1800, 3600};
            static const int tCount = 7;
            int curIdx = 2;
            for (int i = 0; i < tCount; ++i) {
                if (timeouts[i] == config_.idle_timeout_sec) { curIdx = i; break; }
            }
            int nextIdx = (curIdx + step + tCount) % tCount;
            config_.idle_timeout_sec = timeouts[nextIdx];
            ioConfig = config_;
        } else if (row == 3) {
            static constexpr IdleScreenMode modes[] = {IdleScreenMode::Sleep, IdleScreenMode::OledBlack, IdleScreenMode::Dim};
            const auto current = std::find(std::begin(modes), std::end(modes), config_.idle_mode);
            config_.idle_mode = modes[(std::distance(std::begin(modes), current) + step + 3) % 3];
            ioConfig = config_;
        } else if (row == 4) {
            static const int threshs[] = {0, 10, 15, 20, 30};
            static const int thCount = 5;
            int curIdx = 3;
            for (int i = 0; i < thCount; ++i) {
                if (threshs[i] == config_.low_battery_threshold) { curIdx = i; break; }
            }
            int nextIdx = (curIdx + step + thCount) % thCount;
            config_.low_battery_threshold = threshs[nextIdx];
            ioConfig = config_;
        }

    } else if (tab == static_cast<int>(SettingsTab::Update)) {
        if (row == 1 && isClick) {
            updateStatusMsg_ = "GitHub kontrol edildi: Sürümünüz Güncel (" SWITCHDECK_VERSION_TAG ")";
            return SettingsEvent::CheckUpdateRequested;
        }

    } else if (tab == static_cast<int>(SettingsTab::About)) {
        // Informational rows
        return SettingsEvent::None;
    }

    return SettingsEvent::ConfigChanged;
}

SettingsEvent SettingsUI::handleTouch(int screenX, int screenY, AppConfig& ioConfig, DeckState& ioDeck, SDL_Renderer* renderer) {
    // 1. Close Button
    if (screenX >= rectBtnClose_.x && screenX <= rectBtnClose_.x + rectBtnClose_.w &&
        screenY >= rectBtnClose_.y && screenY <= rectBtnClose_.y + rectBtnClose_.h) {
        close();
        return SettingsEvent::None;
    }

    // 2. Tab Selection (Sidebar, 6 tabs)
    for (int i = 0; i < 6; ++i) {
        if (screenX >= tabRects_[i].x && screenX <= tabRects_[i].x + tabRects_[i].w &&
            screenY >= tabRects_[i].y && screenY <= tabRects_[i].y + tabRects_[i].h) {
            activeTab_ = i;
            focusedRow_ = 0;
            return SettingsEvent::None;
        }
    }

    // 3. Row Control Interaction
    for (const auto& rc : currentRowRects_) {
        if (screenX >= rc.cardRect.x && screenX <= rc.cardRect.x + rc.cardRect.w &&
            screenY >= rc.cardRect.y && screenY <= rc.cardRect.y + rc.cardRect.h) {
            focusedRow_ = rc.rowIndex;

            if (rc.btnMinus.w > 0 &&
                screenX >= rc.btnMinus.x && screenX <= rc.btnMinus.x + rc.btnMinus.w &&
                screenY >= rc.btnMinus.y && screenY <= rc.btnMinus.y + rc.btnMinus.h) {
                return adjustRow(activeTab_, rc.rowIndex, -1, false, ioConfig, ioDeck, renderer);
            }
            if (rc.btnPlus.w > 0 &&
                screenX >= rc.btnPlus.x && screenX <= rc.btnPlus.x + rc.btnPlus.w &&
                screenY >= rc.btnPlus.y && screenY <= rc.btnPlus.y + rc.btnPlus.h) {
                return adjustRow(activeTab_, rc.rowIndex, +1, false, ioConfig, ioDeck, renderer);
            }
            if (rc.btnAction.w > 0 &&
                screenX >= rc.btnAction.x && screenX <= rc.btnAction.x + rc.btnAction.w &&
                screenY >= rc.btnAction.y && screenY <= rc.btnAction.y + rc.btnAction.h) {
                return adjustRow(activeTab_, rc.rowIndex, 0, true, ioConfig, ioDeck, renderer);
            }

            return adjustRow(activeTab_, rc.rowIndex, 0, true, ioConfig, ioDeck, renderer);
        }
    }

    return SettingsEvent::None;
}

SettingsEvent SettingsUI::handleGamepad(int button, AppConfig& ioConfig, DeckState& ioDeck, SDL_Renderer* renderer) {
    // B Button: Close Settings
    if (button == SDL_CONTROLLER_BUTTON_B) {
        close();
        return SettingsEvent::None;
    }

    // Switch Tabs with L / R across 6 tabs
    if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
        activeTab_ = (activeTab_ - 1 + 6) % 6;
        focusedRow_ = 0;
        return SettingsEvent::None;
    }
    if (button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
        activeTab_ = (activeTab_ + 1) % 6;
        focusedRow_ = 0;
        return SettingsEvent::None;
    }

    int rowCount = getRowCountForTab(activeTab_);
    if (rowCount <= 0) return SettingsEvent::None;

    // Navigate Rows with D-Pad Up / Down
    if (button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
        focusedRow_ = (focusedRow_ - 1 + rowCount) % rowCount;
        return SettingsEvent::None;
    }
    if (button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        focusedRow_ = (focusedRow_ + 1) % rowCount;
        return SettingsEvent::None;
    }

    // Adjust Value with D-Pad Left / Right
    if (button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
        return adjustRow(activeTab_, focusedRow_, -1, false, ioConfig, ioDeck, renderer);
    }
    if (button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        return adjustRow(activeTab_, focusedRow_, +1, false, ioConfig, ioDeck, renderer);
    }

    // Trigger Action with A Button
    if (button == SDL_CONTROLLER_BUTTON_A) {
        return adjustRow(activeTab_, focusedRow_, 0, true, ioConfig, ioDeck, renderer);
    }

    return SettingsEvent::None;
}
