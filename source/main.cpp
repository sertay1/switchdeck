#include <iostream>
#include <string>
#include <algorithm>
#include <cmath>
#include <optional>
#include <json.hpp>
#include <SDL2/SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#include <switch/services/lbl.h>
#endif

#include "config.hpp"
#include "ws_client.hpp"
#include "deck_state.hpp"
#include "widget_localization.hpp"
#include "ui_renderer.hpp"
#include "updater.hpp"

static bool hasPrimaryUiEvent(const DeckButton& button, const std::string& event) {
    return !button.sessionId.empty() && !button.primaryNodeId.empty() &&
        std::find(button.primaryNodeEvents.begin(), button.primaryNodeEvents.end(), event) != button.primaryNodeEvents.end();
}

#ifdef __SWITCH__
static Result requestConsoleSleep() {
    switch (appletGetAppletType()) {
        case AppletType_LibraryApplet:
        case AppletType_SystemApplet:
        case AppletType_OverlayApplet:
            return appletStartSleepSequence(true);
        default:
            return appletRequestToSleep();
    }
}
#endif

static void triggerDeckAction(WebSocketClient& ws, const DeckButton& button,
                              const char* event, const char* flow, bool configuredFlow) {
    if (!configuredFlow && hasPrimaryUiEvent(button, event)) {
        ws.sendUiEvent(button.sessionId, button.primaryNodeId, event, nullptr, button.revision);
    } else {
        ws.executeAction(button.widgetId, button.folderId, flow);
    }
}

static void triggerMusicPlayerSubButton(WebSocketClient& ws, DeckButton& btn, int subIdx) {
    if (btn.sessionId.empty()) {
        ws.executeAction(btn.widgetId, btn.folderId, "onShortPress");
        return;
    }

    std::string targetNodeId;
    if (!btn.subButtons.empty()) {
        for (const auto& sb : btn.subButtons) {
            if (subIdx == 0 && (sb.iconName.find("prev") != std::string::npos ||
                                sb.iconName.find("back") != std::string::npos ||
                                sb.iconName.find("rewind") != std::string::npos)) {
                targetNodeId = sb.nodeId;
                break;
            }
            if (subIdx == 1 && (sb.iconName.find("play") != std::string::npos ||
                                sb.iconName.find("pause") != std::string::npos)) {
                targetNodeId = sb.nodeId;
                break;
            }
            if (subIdx == 2 && (sb.iconName.find("next") != std::string::npos ||
                                sb.iconName.find("forward") != std::string::npos ||
                                sb.iconName.find("skip") != std::string::npos)) {
                targetNodeId = sb.nodeId;
                break;
            }
        }
        if (targetNodeId.empty()) {
            int clamped = std::clamp(subIdx, 0, static_cast<int>(btn.subButtons.size()) - 1);
            targetNodeId = btn.subButtons[clamped].nodeId;
        }
    } else {
        targetNodeId = btn.primaryNodeId;
    }

    if (!targetNodeId.empty()) {
        btn.pressedUiNode = targetNodeId;
        btn.pressFeedbackSeconds = 0.060f;
        ws.sendUiEvent(btn.sessionId, targetNodeId, "press", nullptr, btn.revision);
    } else {
        ws.executeAction(btn.widgetId, btn.folderId, "onShortPress");
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

#ifdef __SWITCH__
    romfsInit();
    nifmInitialize(NifmServiceType_User);
    csrngInitialize();

    SocketInitConfig sockCfg = *(socketGetDefaultInitConfig());
    sockCfg.num_bsd_sessions = 8;
    socketInitialize(&sockCfg);

    psmInitialize();
    lblInitialize();

    // Direct hardware gamepad initialization via libnx
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
#endif

    AppUpdater::init();

    AppConfig config = ConfigManager::load();

#ifdef __SWITCH__
    float initBrightness = std::clamp(static_cast<float>(config.brightness_pct) / 100.0f, 0.05f, 1.0f);
    lblSetCurrentBrightnessSetting(initBrightness);
    lblApplyCurrentBrightnessSettingToBacklight();
#endif

    UIRenderer ui;
    if (!ui.init()) {
        AppUpdater::cleanup();
        std::cerr << "Failed to initialize UI renderer!\n";
#ifdef __SWITCH__
        lblExit();
        psmExit();
        socketExit();
        csrngExit();
        nifmExit();
        romfsExit();
#endif
        return 1;
    }

    ui.getSettingsUI().init(config);

    DeckState deckState;
    if (config.remember_last_folder && !config.last_folder_id.empty()) {
        deckState.setPreferredInitialFolder(config.last_folder_id);
    } else if (!config.startup_folder_id.empty()) {
        deckState.setPreferredInitialFolder(config.startup_folder_id);
    }

    WebSocketClient ws;

    auto syncSession = [&]() {
        const AppConfig connectedConfig = ws.getCurrentConfig();
        if (config.sameServer(connectedConfig)) {
            config.copySessionFrom(connectedConfig);
            config.startup_profile_id = connectedConfig.startup_profile_id;
        } else {
            config.copySessionFrom(AppConfig{});
        }
        ui.getSettingsUI().syncFromConfig(config);
    };

    std::string appearanceRegularFont, appearanceBoldFont;
    auto connectToDeck = [&]() {
        ws.disconnect();
        if (!config.sameServer(ws.getCurrentConfig())) {
            ui.clearWidgetFontBytes();
            appearanceRegularFont.clear();
            appearanceBoldFont.clear();
            config.startup_profile_id.clear();
            config.startup_folder_id.clear();
            config.last_folder_id.clear();
        }
        syncSession();
        deckState.reset(ui.getRenderer());
        deckState.setPreferredInitialFolder(config.remember_last_folder && !config.last_folder_id.empty()
                                             ? config.last_folder_id : config.startup_folder_id);
        ws.connect(config);
        config.pairing_code.clear();
        ui.getSettingsUI().syncFromConfig(config);
    };

    auto resetSession = [&]() {
        ws.resetSession();
        config.pairing_code.clear();
        syncSession();
        deckState.reset(ui.getRenderer());
    };

    bool resourcesChanged = true;
    auto syncCurrentFolder = [&]() {
        for (const auto& session : ws.syncFolderAndWidgets(deckState.getCurrentFolderId(), deckState.getDisplayedWidgetIds())) {
            deckState.handleUiSessionClosedOrInvalidated(session);
        }
        resourcesChanged = true;
    };

    auto performWidgetNavigation = [&](const DeckButton& btn) -> bool {
        if (btn.navCommand.empty()) return false;
        bool moved = false;
        if (btn.navCommand == "changeTo" && !btn.navTargetFolderId.empty()) {
            moved = deckState.openFolder(btn.navTargetFolderId, ui.getRenderer());
        } else if (btn.navCommand == "parent") {
            moved = deckState.goParentFolder(ui.getRenderer());
        } else if (btn.navCommand == "back") {
            moved = deckState.goBackFolder(ui.getRenderer());
        }
        if (moved) {
            if (config.remember_last_folder) {
                config.last_folder_id = deckState.getCurrentFolderId();
                ws.updateConfig(config);
            }
            syncCurrentFolder();
        }
        return moved;
    };

    ws.setCallbacks(
        [&](const std::string& msg) {
            try {
                auto j = nlohmann::json::parse(msg);
                if (!j.is_object()) return;

                // 1. Internal Macro Deck 3 State Synthesis Messages
                std::string internalType = jsonStr(j, "_internal");
                if (!internalType.empty()) {
                    if (internalType == "MD3_LOCALIZATION") {
                        setWidgetLocalization(j["catalog"]);
                    } else if (internalType == "MD3_APPEARANCE_FONTS") {
                        appearanceRegularFont = jsonStr(j, "regular");
                        appearanceBoldFont = jsonStr(j, "bold");
                        ui.setDefaultWidgetFonts(appearanceRegularFont, appearanceBoldFont);
                        resourcesChanged = true;
                    } else if (internalType == "MD3_DECK") {
                        if (jsonStr(j, "requestedProfileId") != ws.getCurrentConfig().startup_profile_id) return;
                        const auto previousProfile = deckState.getCurrentProfileId();
                        const auto profile = deckState.handleMd3Profiles(j["profiles"], jsonStr(j, "profileId"));
                        deckState.handleMd3Folders(j["folders"], profile, previousProfile != profile, ui.getRenderer());
                        if (profile != config.startup_profile_id) {
                            config.startup_profile_id = profile;
                            ws.updateConfig(config);
                        }
                        syncCurrentFolder();
                    } else if (internalType == "MD3_WIDGET_SESSION") {
                        deckState.registerWidgetSession(jsonStr(j, "widgetId"), jsonStr(j, "sessionId"));
                    } else if (internalType == "MD3_WIDGET_SESSION_ENDED") {
                        deckState.handleUiSessionClosedOrInvalidated(jsonStr(j, "sessionId"));
                    }
                    return;
                }

                // 2. Macro Deck 3 WebSocket Notifications
                if (jsonStr(j, "kind") == "message") {
                    std::string evType = jsonStr(j, "type");
                    const nlohmann::json body = (j.contains("payload") && j["payload"].is_object())
                                                ? j["payload"] : nlohmann::json::object();

                    if (evType == "UiSessionTreeUpdatedEvent") {
                        std::string sid = jsonStr(body, "sessionId");
                        int rev = jsonInt(body, "revision", 0);
                        if (!sid.empty() && body.contains("tree")) {
                            deckState.handleUiSessionTreeUpdated(sid, rev, body["tree"]);
                            resourcesChanged = true;
                        }
                    } else if (evType == "UiSessionPatchedEvent") {
                        std::string sid = jsonStr(body, "sessionId");
                        if (!sid.empty() && body.contains("patch") && body["patch"].is_object()) {
                            if (!deckState.handleUiSessionPatched(sid, body["patch"])) {
                                ws.resyncWidgetSession(sid);
                            } else {
                                resourcesChanged = true;
                            }
                        }
                    } else if (evType == "FolderCreatedEvent" || evType == "FolderUpdatedEvent") {
                        const nlohmann::json& fJson = body.contains("folder") ? body["folder"] : body;
                        deckState.handleMd3FolderUpsert(fJson, ui.getRenderer());
                        syncCurrentFolder();
                    } else if (evType == "FolderDeletedEvent") {
                        deckState.handleMd3FolderDelete(jsonStr(body, "folderId"), ui.getRenderer());
                        syncCurrentFolder();
                    } else if (evType == "WidgetCreatedEvent" || evType == "WidgetUpdatedEvent") {
                        if (body.contains("widget")) {
                            if (deckState.handleMd3WidgetUpsert(jsonStr(body, "folderId"), body["widget"], ui.getRenderer())) {
                                ws.invalidateWidget(jsonStr(body["widget"], "id"));
                            }
                            syncCurrentFolder();
                        }
                    } else if (evType == "WidgetsUpdatedEvent" || evType == "WidgetsCreatedEvent" || evType == "WidgetPositionsUpdatedEvent") {
                        std::string fid = jsonStr(body, "folderId");
                        if (body.contains("widgets") && body["widgets"].is_array()) {
                            for (const auto& w : body["widgets"]) {
                                if (deckState.handleMd3WidgetUpsert(fid, w, ui.getRenderer())) {
                                    ws.invalidateWidget(jsonStr(w, "id"));
                                }
                            }
                            syncCurrentFolder();
                        }
                    } else if (evType == "WidgetDeletedEvent") {
                        deckState.handleMd3WidgetDelete(jsonStr(body, "folderId"), jsonStr(body, "widgetId"), ui.getRenderer());
                        syncCurrentFolder();
                    } else if (evType == "WidgetsDeletedEvent") {
                        std::string fid = jsonStr(body, "folderId");
                        std::vector<std::string> wids;
                        if (body.contains("widgetIds") && body["widgetIds"].is_array()) {
                            for (const auto& widVal : body["widgetIds"]) {
                                if (widVal.is_string()) wids.push_back(widVal.get<std::string>());
                            }
                        }
                        deckState.handleWidgetsDeleted(fid, wids, ui.getRenderer());
                        syncCurrentFolder();
                    } else if (evType == "WidgetStateInvalidatedEvent") {
                        // Live sessions receive their own tree updates and patches.
                        const auto invalidateHidden = [&](const std::string& widgetId) {
                            if (!widgetId.empty() && !ws.hasWidgetSession(widgetId)) {
                                deckState.handleWidgetStateInvalidated(widgetId);
                            }
                        };
                        invalidateHidden(jsonStr(body, "widgetId"));
                        if (body.contains("widgetIds") && body["widgetIds"].is_array()) {
                            for (const auto& item : body["widgetIds"]) {
                                if (item.is_string()) invalidateHidden(item.get<std::string>());
                            }
                        }
                    } else if (evType == "UiSessionInvalidatedEvent" || evType == "UiSessionClosedEvent") {
                        std::string sid = jsonStr(body, "sessionId");
                        if (!sid.empty()) {
                            deckState.handleUiSessionClosedOrInvalidated(sid);
                        }
                    } else if (evType == "AppearanceChangedEvent") {
                        ws.requestAppearanceFonts();
                    } else if (evType == "ProfileUpdatedEvent") {
                        deckState.handleMd3ProfileUpdate(body, ui.getRenderer());
                        ws.requestDeckReload();
                    } else if (evType == "ProfileDeletedEvent") {
                        ws.requestDeckReload();
                    } else if (evType == "FolderNavigationEvent") {
                        std::string cmd = jsonStr(body, "command");
                        bool moved = false;
                        if (cmd == "changeTo") {
                            moved = deckState.openFolder(jsonStr(body, "folderId"), ui.getRenderer());
                        } else if (cmd == "parent") {
                            moved = deckState.goParentFolder(ui.getRenderer());
                        } else if (cmd == "back") {
                            moved = deckState.goBackFolder(ui.getRenderer());
                        }
                        if (moved) {
                            if (config.remember_last_folder) {
                                config.last_folder_id = deckState.getCurrentFolderId();
                                ws.updateConfig(config);
                            }
                            syncCurrentFolder();
                        }
                    }
                    return;
                }

                // 3. Macro Deck 2 Legacy Fallback
                std::string method = jsonStr(j, "Method");
                if (method == "GET_CONFIG") {
                    deckState.handleConfig(j);
                    nlohmann::json req;
                    req["Method"] = "GET_BUTTONS";
                    ws.sendText(req.dump());
                } else if (method == "GET_BUTTONS") {
                    deckState.handleButtons(j, ui.getRenderer());
                } else if (method == "UPDATE_BUTTON") {
                    deckState.handleUpdateButton(j, ui.getRenderer());
                } else if (method == "UPDATE_LABEL") {
                    deckState.handleUpdateLabel(j, ui.getRenderer());
                }
            } catch (...) {}
        },
        [&](WsState state, const std::string& msg) {
            (void)msg;
            if (state == WsState::Connected) {
                const auto connectedConfig = ws.getCurrentConfig();
                if (connectedConfig.port != 8193 && connectedConfig.access_token.empty()) {
                    nlohmann::json handshake;
                    handshake["Method"] = "CONNECTED";
                    handshake["Client-Id"] = connectedConfig.device_name;
                    handshake["API"] = 20;
                    handshake["Device-Type"] = "Web";
                    ws.sendText(handshake.dump());
                }
            }
        },
        [&](const ResourceImageResult& res) {
            if (res.resourceId.rfind("font:", 0) == 0) {
                ui.setWidgetFontBytes(res.resourceId.substr(5), res.rawBytes);
            } else {
                deckState.setWidgetImageBytes(res.widgetId, res.resourceId, res.rawBytes, ui.getRenderer());
            }
        }
    );

    ws.connect(config);

    if (config.access_token.empty() && config.refresh_cookie.empty() && config.pairing_code.empty()) {
        ui.getSettingsUI().open();
    }
    config.pairing_code.clear();

    // GitHub Releases Background Update Check State
    bool showUpdateModal = false;
    std::string updateModalVer;
    bool startupUpdateCheckRequested = false;
    Uint32 bootTick = SDL_GetTicks();
    std::string actionError;
    Uint32 actionErrorUntil = 0;

    // Gesture & Touch State Machine
    int activeTouchRow = -1;
    int activeTouchCol = -1;
    Uint32 touchDownTick = 0;
    bool isDraggingSlider = false;
    bool longPressTriggered = false;
    Uint32 lastSliderAdjustTick = 0;

    struct TouchTarget {
        std::string profileId;
        std::string folderId;
        std::string widgetId;
        int row;
        int col;

        bool operator==(const TouchTarget& other) const {
            return profileId == other.profileId && folderId == other.folderId &&
                   widgetId == other.widgetId && row == other.row && col == other.col;
        }

        bool operator!=(const TouchTarget& other) const { return !(*this == other); }
    };

    auto touchTargetAt = [&](int row, int col) -> std::optional<TouchTarget> {
        if (const auto* button = deckState.getButtonConst(row, col)) {
            return TouchTarget{deckState.getCurrentProfileId(), deckState.getCurrentFolderId(),
                               button->widgetId, row, col};
        }
        return std::nullopt;
    };

    std::optional<TouchTarget> activeTouchTarget;
    struct PendingTimerPress {
        TouchTarget target;
        const char* event;
        Uint32 deadline;
    };
    std::optional<PendingTimerPress> pendingTimerPress;
    const auto usesTimerSession = [&](const DeckButton& button) {
        return !button.sessionId.empty() || (!button.widgetId.empty() && ws.isConnected());
    };
    const auto sendTimerPress = [&](const DeckButton& button, int row, int col, const char* event) {
        if (hasPrimaryUiEvent(button, event)) {
            ws.sendUiEvent(button.sessionId, button.primaryNodeId, event, nullptr, button.revision);
        } else if (button.primaryNodeId.empty()) {
            if (const auto target = touchTargetAt(row, col)) {
                pendingTimerPress = PendingTimerPress{*target, event, SDL_GetTicks() + 1500};
            }
        }
    };
    std::string activeTouchSession;
    SDL_TouchID activeTouchDevice = 0;
    SDL_FingerID activeFinger = 0;
    struct TouchRelease {
        std::string widget;
        std::string session;
        std::string node;
        int revision;
    };
    std::optional<TouchRelease> touchRelease;
    const auto endTouch = [&]() {
        if (touchRelease) {
            const auto* button = deckState.getButtonById(touchRelease->widget);
            const int revision = button && button->sessionId == touchRelease->session
                ? button->revision : touchRelease->revision;
            ws.sendUiEvent(touchRelease->session, touchRelease->node, "press-end", nullptr, revision);
            touchRelease.reset();
        }
        activeTouchTarget.reset();
        activeTouchSession.clear();
    };
    const auto cancelTouch = [&]() {
        if (!activeTouchTarget) return;
        if (touchTargetAt(activeTouchRow, activeTouchCol) == activeTouchTarget) {
            deckState.setButtonState(activeTouchRow, activeTouchCol, false);
        }
        endTouch();
    };
    std::optional<std::string> activeTreeButton;
    std::optional<UIRenderer::LevelControl> activeLevelControl;
    SDL_Point levelTouchStart{}, levelPrevious{};
    float rawTreeLevel = 0;
    bool levelMoved = false;
    bool pendingLevelAdjust = false;
    std::string lastLevelTapWidget, lastLevelTapNode;
    Uint32 lastLevelTapTick = 0;
    struct PendingPress {
        TouchTarget target;
        int subButton;
        Uint32 deadline;
    };
    std::optional<PendingPress> pendingPress;

    // Power, Idle & Battery Tracking
    Uint32 lastActivityTick = SDL_GetTicks();
    Uint32 lastBatCheckTick = 0;
    int batteryPct = 100;
    bool isCharging = false;
    bool idleSleepAttempted = false;
    const auto noteActivity = [&](Uint32 tick) {
        lastActivityTick = tick;
        idleSleepAttempted = false;
    };

#ifdef __SWITCH__
    // Let SwitchDeck's timer choose the idle action independently of the system timer.
    appletSetAutoSleepDisabled(true);
    appletSetMediaPlaybackState(true);
    bool resumed = false;
    AppletHookCookie resumeHook{};
    appletHook(&resumeHook, [](AppletHookType type, void* context) {
        if (type == AppletHookType_OnResume) *static_cast<bool*>(context) = true;
    }, &resumed);
#endif

    // Analog stick repeat timing
    Uint32 lastStickMoveTick = 0;

    auto fireShortPress = [&](int r, int c, int subIdx) {
        DeckButton* b = deckState.getButton(r, c);
        if (!b) return;

        if (performWidgetNavigation(*b)) {
            return;
        }

        if (b->widgetType == "MusicPlayer") {
            const int control = subIdx >= 0 ? subIdx : 1;
            const auto* spotifyPlay = control == 1 ? deckState.findSpotifyPlayAction(*b) : nullptr;
            if (spotifyPlay) {
                ws.executeAction(spotifyPlay->widgetId, spotifyPlay->folderId, "onShortPress");
            } else {
                triggerMusicPlayerSubButton(ws, *b, control);
            }
        } else if (b->widgetType == "Slider") {
            std::string nid = !b->sliderNodeId.empty() ? b->sliderNodeId : (!b->primaryNodeId.empty() ? b->primaryNodeId : "root");
            if (!b->sessionId.empty()) {
                ws.sendUiEvent(b->sessionId, nid, "change", b->sliderLevel, b->revision);
            }
        } else if (b->widgetType == "Countdown") {
            if (usesTimerSession(*b)) {
                sendTimerPress(*b, r, c, "press");
                return;
            }
            Uint64 nowTicks64 = static_cast<Uint64>(SDL_GetTicks());
            if (b->countdownIsFinished) {
                b->countdownIsFinished = false;
                b->countdownIsPaused = false;
                b->countdownIsRunning = false;
                b->hasAccentColor = false;
                b->accentR = 33; b->accentG = 150; b->accentB = 243;
                b->progressPositionMs = 0.0;
                b->progressRate = 0.0;
                b->countdownRingLevel = 1.0f;
                b->progressAnchorTicks = nowTicks64;
            } else if (b->progressRate > 0.0) {
                b->progressPositionMs = b->getCurrentProgressMs(nowTicks64, true);
                b->progressRate = 0.0;
                b->countdownIsPaused = true;
                b->countdownIsRunning = false;
                b->progressAnchorTicks = nowTicks64;
            } else {
                b->countdownIsPaused = false;
                b->countdownIsRunning = true;
                b->hasAccentColor = false;
                b->accentR = 33; b->accentG = 150; b->accentB = 243;
                b->progressRate = 1.0;
                b->progressAnchorTicks = nowTicks64;
            }
        } else if (b->widgetType == "Stopwatch") {
            if (usesTimerSession(*b)) {
                sendTimerPress(*b, r, c, "press");
                return;
            }
            Uint64 nowTicks64 = static_cast<Uint64>(SDL_GetTicks());
            if (b->progressRate > 0.0) {
                b->progressPositionMs = b->getCurrentProgressMs(nowTicks64, false);
                b->progressRate = 0.0;
                b->stopwatchIsPaused = true;
                b->stopwatchIsRunning = false;
                b->progressAnchorTicks = nowTicks64;
            } else {
                b->stopwatchIsPaused = false;
                b->stopwatchIsRunning = true;
                b->progressRate = 1.0;
                b->progressAnchorTicks = nowTicks64;
            }
        } else if (!b->widgetId.empty()) {
            triggerDeckAction(ws, *b, "press", "onShortPress", b->hasShortPressFlow);
        } else {
            nlohmann::json req;
            req["Method"] = "BUTTON_RELEASE";
            req["Message"] = std::to_string(r) + "_" + std::to_string(c);
            ws.sendText(req.dump());
        }
    };

    const auto applyLevel = [](DeckButton& button, float level, Uint32 tick) {
        button.sliderLevel = level;
        button.visualSliderLevel = level;
        button.sliderValueText = std::to_string(static_cast<int>(std::lround(level * 100))) + " %";
        button.lastLocalSlideTick = tick;
    };

    auto adjustSliderAtFocus = [&](float delta) {
        DeckButton* sb = deckState.getButton(ui.getFocusRow(), ui.getFocusCol());
        if (sb && sb->widgetType == "Slider") {
            float newLvl = std::clamp(sb->sliderLevel + delta, 0.0f, 1.0f);
            sb->sliderLevel = newLvl;
            sb->visualSliderLevel = newLvl;
            int pct = static_cast<int>(std::round(newLvl * 100.0f));
            sb->sliderValueText = std::to_string(pct) + " %";
            sb->lastLocalSlideTick = static_cast<Uint32>(SDL_GetTicks());
            std::string nid = !sb->sliderNodeId.empty() ? sb->sliderNodeId : (!sb->primaryNodeId.empty() ? sb->primaryNodeId : "root");
            if (!sb->sessionId.empty()) {
                ws.sendUiEvent(sb->sessionId, nid, "adjust", newLvl, sb->revision);
                ws.sendUiEvent(sb->sessionId, nid, "change", newLvl, sb->revision);
            }
        }
    };

    Uint32 lastRenderTick = 0;
    Uint32 lastResourceScan = 0;
    Uint32 lastSessionSync = 0;
    int lastRenderedDim = -1;
    bool running = true;
    while (running) {
#ifdef __SWITCH__
        if (!appletMainLoop()) {
            break;
        }
#endif

        Uint32 nowTick = SDL_GetTicks();
#ifdef __SWITCH__
        if (resumed) {
            resumed = false;
            cancelTouch();
            noteActivity(nowTick);
        }
#endif

        // 1. Check auto-discovery result
        std::string discHost;
        int discPort = 8193;
        if (ws.consumeDiscoveredServer(discHost, discPort) && ui.getSettingsUI().isOpen()) {
            if (discHost != config.host || discPort != config.port) config.pairing_code.clear();
            config.host = discHost;
            config.port = discPort;
            syncSession();
        }

        // 2. Poll battery & charger status every 2 seconds
        if (nowTick - lastBatCheckTick >= 2000) {
            lastBatCheckTick = nowTick;
#ifdef __SWITCH__
            u32 bPct = 100;
            PsmChargerType chType = PsmChargerType_Unconnected;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&bPct))) {
                batteryPct = static_cast<int>(bPct);
            }
            if (R_SUCCEEDED(psmGetChargerType(&chType))) {
                isCharging = (chType != PsmChargerType_Unconnected);
            }
#endif
        }

        ws.poll();
        if (nowTick - lastSessionSync >= 250) {
            lastSessionSync = nowTick;
            syncSession();
        }
        const Uint32 resourceInterval = resourcesChanged ? 125 : 2000;
        if (ws.isConnected() && nowTick - lastResourceScan >= resourceInterval) {
            lastResourceScan = nowTick;
            resourcesChanged = false;
            for (const auto* face : {&appearanceRegularFont, &appearanceBoldFont}) {
                if (!face->empty() && !ui.hasWidgetFont(*face)) ws.requestResourceImage("appearance", "font:" + *face);
            }
            for (const auto& request : deckState.getPendingResourceRequests()) {
                if (request.second.rfind("font:", 0) == 0 && ui.hasWidgetFont(request.second.substr(5))) continue;
                const auto* button = deckState.getButtonById(request.first);
                const bool musicCover = button && !button->musicArtworkId.empty() && button->resourceId == request.second;
                ws.requestResourceImage(request.first, request.second, musicCover);
            }
        }

        if (pendingTimerPress) {
            const auto& target = pendingTimerPress->target;
            if (SDL_TICKS_PASSED(nowTick, pendingTimerPress->deadline) || ui.getSettingsUI().isOpen() ||
                touchTargetAt(target.row, target.col) != std::optional<TouchTarget>{target}) {
                pendingTimerPress.reset();
            } else if (const auto* button = deckState.getButtonConst(target.row, target.col);
                       button && hasPrimaryUiEvent(*button, pendingTimerPress->event)) {
                ws.sendUiEvent(button->sessionId, button->primaryNodeId, pendingTimerPress->event,
                               nullptr, button->revision);
                pendingTimerPress.reset();
            }
        }

        if (activeTouchTarget) {
            const DeckButton* button = deckState.getButtonConst(activeTouchRow, activeTouchCol);
            if (touchTargetAt(activeTouchRow, activeTouchCol) != activeTouchTarget || ui.getSettingsUI().isOpen() ||
                (button && !activeTouchSession.empty() && activeTouchSession != button->sessionId)) {
                cancelTouch();
            }
        }
        if (pendingPress) {
            const auto& target = pendingPress->target;
            if (ui.getSettingsUI().isOpen() || touchTargetAt(target.row, target.col) != std::optional<TouchTarget>{target}) {
                pendingPress.reset();
            } else if (SDL_TICKS_PASSED(nowTick, pendingPress->deadline)) {
                fireShortPress(target.row, target.col, pendingPress->subButton);
                pendingPress.reset();
            }
        }

        // 4. Check Long Press Hold Timer while finger is down
        if (activeTouchTarget && !activeTreeButton && !isDraggingSlider && !longPressTriggered) {
            if (nowTick - touchDownTick >= static_cast<Uint32>(config.long_press_ms)) {
                DeckButton* b = deckState.getButton(activeTouchRow, activeTouchCol);
                if (b && !b->widgetId.empty() &&
                    (b->hasLongPressFlow || b->widgetType == "Countdown" || b->widgetType == "Stopwatch")) {
                    longPressTriggered = true;
                    const bool timer = b->widgetType == "Countdown" || b->widgetType == "Stopwatch";
                    if (!timer) {
                        triggerDeckAction(ws, *b, "long-press", "onLongPress", b->hasLongPressFlow);
                    } else if (usesTimerSession(*b)) {
                        sendTimerPress(*b, activeTouchRow, activeTouchCol, "long-press");
                    }
                    if (!usesTimerSession(*b) && b->widgetType == "Countdown") {
                        b->countdownIsFinished = false;
                        b->countdownIsPaused = false;
                        b->countdownIsRunning = false;
                        b->hasAccentColor = false;
                        b->accentR = 33; b->accentG = 150; b->accentB = 243;
                        b->progressPositionMs = 0.0;
                        b->progressRate = 0.0;
                        b->countdownRingLevel = 1.0f;
                        b->progressAnchorTicks = nowTick;
                    } else if (!usesTimerSession(*b) && b->widgetType == "Stopwatch") {
                        b->stopwatchTimeText = "0:00";
                        b->stopwatchIsRunning = false;
                        b->stopwatchIsPaused = false;
                        b->progressPositionMs = 0.0;
                        b->progressRate = 0.0;
                        b->progressAnchorTicks = nowTick;
                    }
                }
            }
        }

        // 4.5. Poll Background App Updater (100% thread-safe on main thread)
        UpdateCheckResult updRes;
        if (AppUpdater::poll(updRes)) {
            if (updRes.hasUpdate) {
                showUpdateModal = true;
                cancelTouch();
                pendingPress.reset();
                updateModalVer = updRes.latestVersion;
                ui.getSettingsUI().setUpdateStatus("Yeni Güncelleme Mevcut! (" + updRes.latestVersion + ")");
            } else if (updRes.error) {
                ui.getSettingsUI().setUpdateStatus("Hata: " + updRes.message);
            } else {
                ui.getSettingsUI().setUpdateStatus(updRes.message);
            }
        }

        // Trigger startup update check smoothly after 3.5 seconds (so app boots instantly)
        if (!startupUpdateCheckRequested && (nowTick - bootTick >= 3500)) {
            startupUpdateCheckRequested = true;
            AppUpdater::requestCheck(SWITCHDECK_VERSION_TAG);
        }

        // 5. Calculate Screen Dim Alpha
        int dimAlpha = 0;
        const auto idleTimeout = static_cast<Uint32>(config.idle_timeout_sec * 1000);
        if (config.idle_enabled && nowTick - lastActivityTick >= idleTimeout) {
            if (config.idle_mode == IdleScreenMode::OledBlack) dimAlpha = 255;
            if (config.idle_mode == IdleScreenMode::Dim) dimAlpha = 215;
        }

        // 6. Direct Hardware Joy-Con Input via libnx
#ifdef __SWITCH__
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        HidAnalogStickState stickL = padGetStickPos(&pad, 0);

        const bool hadGamepadInput = kDown != 0 || std::abs(stickL.x) > 18000 || std::abs(stickL.y) > 18000;
        const bool wokeFromIdle = dimAlpha > 0 && hadGamepadInput;
        if (hadGamepadInput) noteActivity(nowTick);
        if (wokeFromIdle) {
            dimAlpha = 0;
        }

        const bool modalWasOpen = showUpdateModal;
        if (!wokeFromIdle && modalWasOpen &&
            (kDown & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus |
                      HidNpadButton_Minus | HidNpadButton_X | HidNpadButton_Y))) {
            showUpdateModal = false;
        }

        if (wokeFromIdle || modalWasOpen) {
            kDown = 0;
            stickL = {};
        }

        // Plus (+): Exit application immediately
        if (kDown & HidNpadButton_Plus) {
            running = false;
            break;
        }

        // Minus (-): Toggle Settings menu
        if (kDown & HidNpadButton_Minus) {
            if (ui.getSettingsUI().isOpen()) {
                ui.getSettingsUI().close();
            } else {
                ui.getSettingsUI().syncFromConfig(config);
                ui.getSettingsUI().open();
            }
        }

        if (ui.getSettingsUI().isOpen()) {
            // Forward Joy-Con input to Settings UI
            SettingsEvent sev = SettingsEvent::None;
            if (kDown & HidNpadButton_B) {
                ui.getSettingsUI().close();
            } else if (kDown & HidNpadButton_L) {
                sev = ui.getSettingsUI().handleGamepad(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, config, deckState, ui.getRenderer());
            } else if (kDown & HidNpadButton_R) {
                sev = ui.getSettingsUI().handleGamepad(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, config, deckState, ui.getRenderer());
            } else if (kDown & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                sev = ui.getSettingsUI().handleGamepad(SDL_CONTROLLER_BUTTON_DPAD_UP, config, deckState, ui.getRenderer());
            } else if (kDown & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                sev = ui.getSettingsUI().handleGamepad(SDL_CONTROLLER_BUTTON_DPAD_DOWN, config, deckState, ui.getRenderer());
            } else if (kDown & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_ZL)) {
                sev = ui.getSettingsUI().handleGamepad(SDL_CONTROLLER_BUTTON_DPAD_LEFT, config, deckState, ui.getRenderer());
            } else if (kDown & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_ZR)) {
                sev = ui.getSettingsUI().handleGamepad(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, config, deckState, ui.getRenderer());
            } else if (kDown & HidNpadButton_A) {
                sev = ui.getSettingsUI().handleGamepad(SDL_CONTROLLER_BUTTON_A, config, deckState, ui.getRenderer());
            }

            if (sev == SettingsEvent::ConnectRequested) {
                ui.getSettingsUI().close();
                connectToDeck();
            } else if (sev == SettingsEvent::ResetSessionRequested) {
                resetSession();
            } else if (sev == SettingsEvent::AutoDiscoverRequested) {
                ws.startAutoDiscovery(config.port);
            } else if (sev == SettingsEvent::ReloadDeckRequested) {
                ws.requestDeckReload();
            } else if (sev == SettingsEvent::ProfileChanged) {
                ws.updateConfig(config);
                ws.requestDeckReload();
            } else if (sev == SettingsEvent::FolderChanged) {
                ws.updateConfig(config);
                syncCurrentFolder();
            } else if (sev == SettingsEvent::GridSettingsChanged || sev == SettingsEvent::ConfigChanged) {
                ws.updateConfig(config);
            } else if (sev == SettingsEvent::CheckUpdateRequested) {
                ui.getSettingsUI().setUpdateStatus("GitHub denetleniyor...");
                AppUpdater::requestCheck(SWITCHDECK_VERSION_TAG);
            }
        } else {
            // Deck View Joy-Con Controls
            int curRows = deckState.getRows();
            int curCols = deckState.getCols();

            // Analog stick navigation with debounce
            if (nowTick - lastStickMoveTick >= 170) {
                if (stickL.y > 18000) {
                    ui.moveFocus(-1, 0, curRows, curCols);
                    lastStickMoveTick = nowTick;
                } else if (stickL.y < -18000) {
                    ui.moveFocus(1, 0, curRows, curCols);
                    lastStickMoveTick = nowTick;
                } else if (stickL.x > 18000) {
                    ui.moveFocus(0, 1, curRows, curCols);
                    lastStickMoveTick = nowTick;
                } else if (stickL.x < -18000) {
                    ui.moveFocus(0, -1, curRows, curCols);
                    lastStickMoveTick = nowTick;
                }
            }

            // D-Pad Directional Navigation
            if (kDown & HidNpadButton_Up) {
                ui.moveFocus(-1, 0, curRows, curCols);
            } else if (kDown & HidNpadButton_Down) {
                ui.moveFocus(1, 0, curRows, curCols);
            } else if (kDown & HidNpadButton_Left) {
                ui.moveFocus(0, -1, curRows, curCols);
            } else if (kDown & HidNpadButton_Right) {
                ui.moveFocus(0, 1, curRows, curCols);
            }

            // A: Activate / Click focused button
            if (kDown & HidNpadButton_A) {
                const int row = ui.getFocusRow();
                const int col = ui.getFocusCol();
                if (auto* button = deckState.getButton(row, col); button && button->widgetType == "ActionButton") {
                    button->pressFeedbackSeconds = 0.060f;
                }
                fireShortPress(row, col, -1);
            }

            // B: Back folder (if inside subfolder, return to parent)
            if (kDown & HidNpadButton_B) {
                if (deckState.canGoBackOrParent()) {
                    if (deckState.goParentFolder(ui.getRenderer())) {
                        if (config.remember_last_folder) {
                            config.last_folder_id = deckState.getCurrentFolderId();
                            ws.updateConfig(config);
                        }
                        syncCurrentFolder();
                    }
                }
            }

            // L: Previous Page / Folder
            if (kDown & HidNpadButton_L) {
                if (deckState.prevFolder(ui.getRenderer())) {
                    if (config.remember_last_folder) {
                        config.last_folder_id = deckState.getCurrentFolderId();
                        ws.updateConfig(config);
                    }
                    syncCurrentFolder();
                }
            }

            // R: Next Page / Folder
            if (kDown & HidNpadButton_R) {
                if (deckState.nextFolder(ui.getRenderer())) {
                    if (config.remember_last_folder) {
                        config.last_folder_id = deckState.getCurrentFolderId();
                        ws.updateConfig(config);
                    }
                    syncCurrentFolder();
                }
            }

            // Y: Reload deck / reconnect (Ana menü için yenileme tuşu)
            if (kDown & HidNpadButton_Y) {
                if (ws.isConnected()) {
                    ws.requestDeckReload();
                } else {
                    connectToDeck();
                }
            }

            // X: Open Settings Menu (Ayarlara girme tuşu)
            if (kDown & HidNpadButton_X) {
                ui.getSettingsUI().syncFromConfig(config);
                ui.getSettingsUI().open();
            }

            // ZL / ZR: Adjust focused slider
            if (kDown & HidNpadButton_ZL) {
                adjustSliderAtFocus(-0.05f);
            }
            if (kDown & HidNpadButton_ZR) {
                adjustSliderAtFocus(+0.05f);
            }
        }
#endif

        // 7. Touch Events via SDL
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (showUpdateModal && (event.type == SDL_FINGERMOTION || event.type == SDL_FINGERUP)) continue;
            if (event.type == SDL_FINGERDOWN || event.type == SDL_KEYDOWN) {
                noteActivity(nowTick);
                if (dimAlpha > 0) {
                    dimAlpha = 0;
                    continue;
                }
            }

            switch (event.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) cancelTouch();
                    break;

                case SDL_FINGERDOWN: {
                    if (activeTouchTarget && event.tfinger.touchId == activeTouchDevice &&
                        event.tfinger.fingerId == activeFinger) cancelTouch();
                    int screenX = static_cast<int>(event.tfinger.x * 1280.0f);
                    int screenY = static_cast<int>(event.tfinger.y * 720.0f);
                    ui.setShowFocusHighlight(false);

                    if (showUpdateModal) {
                        showUpdateModal = false;
                        break;
                    }

                    if (ui.getSettingsUI().isOpen()) {
                        SettingsEvent sev = ui.getSettingsUI().handleTouch(screenX, screenY, config, deckState, ui.getRenderer());
                        if (sev == SettingsEvent::ConnectRequested) {
                            ui.getSettingsUI().close();
                            connectToDeck();
                        } else if (sev == SettingsEvent::ResetSessionRequested) {
                            resetSession();
                        } else if (sev == SettingsEvent::AutoDiscoverRequested) {
                            ws.startAutoDiscovery(config.port);
                        } else if (sev == SettingsEvent::ReloadDeckRequested) {
                            ws.requestDeckReload();
                        } else if (sev == SettingsEvent::ProfileChanged) {
                            ws.updateConfig(config);
                            ws.requestDeckReload();
                        } else if (sev == SettingsEvent::FolderChanged) {
                            ws.updateConfig(config);
                            syncCurrentFolder();
                        } else if (sev == SettingsEvent::GridSettingsChanged || sev == SettingsEvent::ConfigChanged) {
                            ws.updateConfig(config);
                        } else if (sev == SettingsEvent::CheckUpdateRequested) {
                            ui.getSettingsUI().setUpdateStatus("GitHub denetleniyor...");
                            AppUpdater::requestCheck(SWITCHDECK_VERSION_TAG);
                        }
                        break;
                    }

                    // Check Top Status Bar / Header Buttons
                    if (ui.isSettingsButtonTouched(screenX, screenY, config)) {
                        ui.getSettingsUI().syncFromConfig(config);
                        ui.getSettingsUI().open();
                        break;
                    }

                    if (ui.isFolderPrevTouched(screenX, screenY, deckState)) {
                        if (deckState.prevFolder(ui.getRenderer())) {
                            if (config.remember_last_folder) {
                                config.last_folder_id = deckState.getCurrentFolderId();
                                ws.updateConfig(config);
                            }
                            syncCurrentFolder();
                        }
                        break;
                    }

                    if (ui.isFolderNextTouched(screenX, screenY, deckState)) {
                        if (deckState.nextFolder(ui.getRenderer())) {
                            if (config.remember_last_folder) {
                                config.last_folder_id = deckState.getCurrentFolderId();
                                ws.updateConfig(config);
                            }
                            syncCurrentFolder();
                        }
                        break;
                    }

                    if (!config.show_status_bar && ui.isBackButtonTouched(screenX, screenY, deckState)) {
                        if (deckState.goParentFolder(ui.getRenderer())) {
                            if (config.remember_last_folder) {
                                config.last_folder_id = deckState.getCurrentFolderId();
                                ws.updateConfig(config);
                            }
                            syncCurrentFolder();
                        }
                        break;
                    }

                    if (activeTouchTarget) break;
                    int r = -1, c = -1;
                    if (ui.getButtonAt(screenX, screenY, deckState, config, r, c)) {
                        activeTouchTarget = touchTargetAt(r, c);
                        if (!activeTouchTarget) break;
                        activeTouchDevice = event.tfinger.touchId;
                        activeFinger = event.tfinger.fingerId;
                        activeTreeButton.reset();
                        activeLevelControl.reset();
                        levelMoved = false;
                        pendingLevelAdjust = false;
                        ui.setFocus(r, c);
                        activeTouchRow = r;
                        activeTouchCol = c;
                        touchDownTick = nowTick;
                        longPressTriggered = false;
                        isDraggingSlider = false;

                        DeckButton* btn = deckState.getButton(r, c);
                        if (!btn) break;
                        activeTouchSession = btn->sessionId;

                        const auto treeLevel = ui.getTreeLevelControlAt(screenX, screenY, *btn, deckState, config);
                        if (treeLevel && !treeLevel->id.empty()) {
                            activeLevelControl = treeLevel;
                            levelTouchStart = levelPrevious = {screenX, screenY};
                            rawTreeLevel = treeLevel->valueAt(levelPrevious, levelPrevious, treeLevel->level, true);
                            btn->activeLevelNode = treeLevel->id;
                            applyLevel(*btn, treeLevel->snap(rawTreeLevel), nowTick);
                            deckState.setButtonState(r, c, true);
                            isDraggingSlider = true;
                            if (!treeLevel->relative && treeLevel->adjusts && !btn->sessionId.empty()) {
                                ws.sendUiEvent(btn->sessionId, treeLevel->id, "adjust", btn->sliderLevel, btn->revision);
                            }
                            lastSliderAdjustTick = nowTick;
                        } else if (btn->widgetType == "MusicPlayer" ||
                                   (btn->widgetType != "ActionButton" && btn->widgetType != "Countdown" &&
                                    btn->widgetType != "Stopwatch" && treeLevel)) {
                            activeTreeButton = ui.getWidgetTreeButtonAt(screenX, screenY, *btn, deckState, config);
                            const int subIdx = !activeTreeButton && btn->widgetType == "MusicPlayer"
                                ? ui.getMusicSubButtonAt(screenX, screenY, *btn, deckState, config) : -1;
                            btn->pressedUiNode = activeTreeButton.value_or("");
                            deckState.setButtonState(r, c, !activeTreeButton || !activeTreeButton->empty(), subIdx);
                        } else if (btn->widgetType == "Slider") {
                            deckState.setButtonState(r, c, true);
                            float newLvl = ui.getSliderLevelAt(screenX, screenY, *btn, deckState, config);
                            applyLevel(*btn, newLvl, nowTick);
                            isDraggingSlider = true;
                            std::string nid = !btn->sliderNodeId.empty() ? btn->sliderNodeId : (!btn->primaryNodeId.empty() ? btn->primaryNodeId : "root");
                            if (!btn->sessionId.empty()) {
                                ws.sendUiEvent(btn->sessionId, nid, "adjust", newLvl, btn->revision);
                            }
                            lastSliderAdjustTick = nowTick;
                        } else {
                            deckState.setButtonState(r, c, true);
                            if (!btn->widgetId.empty()) {
                                if (hasPrimaryUiEvent(*btn, "press-end")) {
                                    touchRelease = TouchRelease{btn->widgetId, btn->sessionId, btn->primaryNodeId, btn->revision};
                                }
                                if (hasPrimaryUiEvent(*btn, "press-start")) {
                                    ws.sendUiEvent(btn->sessionId, btn->primaryNodeId, "press-start", nullptr, btn->revision);
                                }
                            } else {
                                nlohmann::json req;
                                req["Method"] = "BUTTON_PRESS";
                                req["Message"] = std::to_string(r) + "_" + std::to_string(c);
                                ws.sendText(req.dump());
                            }
                        }
                    }
                    break;
                }

                case SDL_FINGERMOTION: {
                    if (!ui.getSettingsUI().isOpen() && isDraggingSlider && activeTouchTarget &&
                        activeTouchDevice == event.tfinger.touchId && activeFinger == event.tfinger.fingerId &&
                        touchTargetAt(activeTouchRow, activeTouchCol) == activeTouchTarget) {
                        int screenX = static_cast<int>(event.tfinger.x * 1280.0f);
                        int screenY = static_cast<int>(event.tfinger.y * 720.0f);
                        DeckButton* btn = deckState.getButton(activeTouchRow, activeTouchCol);
                        if (btn && activeLevelControl) {
                            const auto& control = *activeLevelControl;
                            const SDL_Point pointer{screenX, screenY};
                            if (levelMoved || std::abs(pointer.x - levelTouchStart.x) > 4 ||
                                              std::abs(pointer.y - levelTouchStart.y) > 4) {
                                levelMoved = true;
                                rawTreeLevel = control.valueAt(pointer, levelPrevious, rawTreeLevel, false);
                                levelPrevious = pointer;
                                const float next = control.snap(rawTreeLevel);
                                if (next != btn->sliderLevel) {
                                    btn->activeLevelNode = control.id;
                                    applyLevel(*btn, next, nowTick);
                                    pendingLevelAdjust = true;
                                }
                                if (control.adjusts && pendingLevelAdjust && nowTick - lastSliderAdjustTick >= 16 &&
                                    !btn->sessionId.empty()) {
                                    ws.sendUiEvent(btn->sessionId, control.id, "adjust", btn->sliderLevel, btn->revision);
                                    pendingLevelAdjust = false;
                                    lastSliderAdjustTick = nowTick;
                                }
                            }
                        } else if (btn && btn->widgetType == "Slider") {
                            const float newLvl = ui.getSliderLevelAt(screenX, screenY, *btn, deckState, config);
                            applyLevel(*btn, newLvl, nowTick);
                            if (nowTick - lastSliderAdjustTick >= 16) {
                                const std::string nid = !btn->sliderNodeId.empty() ? btn->sliderNodeId :
                                    (!btn->primaryNodeId.empty() ? btn->primaryNodeId : "root");
                                if (!btn->sessionId.empty()) {
                                    ws.sendUiEvent(btn->sessionId, nid, "adjust", newLvl, btn->revision);
                                }
                                lastSliderAdjustTick = nowTick;
                            }
                        }
                    }
                    break;
                }

                case SDL_FINGERUP: {
                    if (!activeTouchTarget || activeTouchDevice != event.tfinger.touchId ||
                        activeFinger != event.tfinger.fingerId) break;
                    const auto releasedTarget = *activeTouchTarget;
                    endTouch();
                    if (!ui.getSettingsUI().isOpen() &&
                        touchTargetAt(activeTouchRow, activeTouchCol) == std::optional<TouchTarget>{releasedTarget}) {
                        int r = activeTouchRow;
                        int c = activeTouchCol;
                        DeckButton* btn = deckState.getButton(r, c);
                        int subIdx = btn ? btn->activeSubBtnIndex : -1;
                        deckState.setButtonState(r, c, false);

                        if (btn) {
                            if (activeLevelControl) {
                                const auto& control = *activeLevelControl;
                                if (!btn->sessionId.empty()) {
                                    if (control.adjusts && pendingLevelAdjust) {
                                        ws.sendUiEvent(btn->sessionId, control.id, "adjust", btn->sliderLevel, btn->revision);
                                    }
                                    if (control.changes && (!control.relative || levelMoved)) {
                                        ws.sendUiEvent(btn->sessionId, control.id, "change", btn->sliderLevel, btn->revision);
                                    }
                                    if (control.doublePress && !levelMoved) {
                                        if (lastLevelTapWidget == btn->widgetId && lastLevelTapNode == control.id &&
                                            nowTick - lastLevelTapTick <= static_cast<Uint32>(config.double_tap_ms)) {
                                            ws.sendUiEvent(btn->sessionId, control.id, "double-press", nullptr, btn->revision);
                                            lastLevelTapWidget.clear();
                                            lastLevelTapNode.clear();
                                        } else {
                                            lastLevelTapWidget = btn->widgetId;
                                            lastLevelTapNode = control.id;
                                            lastLevelTapTick = nowTick;
                                        }
                                    }
                                }
                            } else if (activeTreeButton) {
                                const int x = static_cast<int>(event.tfinger.x * 1280.0f);
                                const int y = static_cast<int>(event.tfinger.y * 720.0f);
                                if (!activeTreeButton->empty() &&
                                    ui.getWidgetTreeButtonAt(x, y, *btn, deckState, config) == activeTreeButton) {
                                    if (*activeTreeButton == "switchdeck:spotify-play") {
                                        fireShortPress(r, c, 1);
                                    } else if (!btn->sessionId.empty()) {
                                        ws.sendUiEvent(btn->sessionId, *activeTreeButton, "press", nullptr, btn->revision);
                                    }
                                }
                            } else if (isDraggingSlider) {
                                // Final slider release -> send adjust & change
                                std::string nid = !btn->sliderNodeId.empty() ? btn->sliderNodeId : (!btn->primaryNodeId.empty() ? btn->primaryNodeId : "root");
                                if (!btn->sessionId.empty()) {
                                    ws.sendUiEvent(btn->sessionId, nid, "adjust", btn->sliderLevel, btn->revision);
                                    ws.sendUiEvent(btn->sessionId, nid, "change", btn->sliderLevel, btn->revision);
                                }
                                btn->lastLocalSlideTick = nowTick;
                            } else if (!longPressTriggered) {
                                if (pendingPress && pendingPress->target == releasedTarget) {
                                    pendingPress.reset();
                                    triggerDeckAction(ws, *btn, "double-press", "onDoublePress", btn->hasDoublePressFlow);
                                } else {
                                    if (pendingPress) {
                                        const auto previous = *pendingPress;
                                        pendingPress.reset();
                                        if (touchTargetAt(previous.target.row, previous.target.col) ==
                                            std::optional<TouchTarget>{previous.target}) {
                                            fireShortPress(previous.target.row, previous.target.col, previous.subButton);
                                        }
                                    }

                                    // An earlier pending action may have navigated away.
                                    if (touchTargetAt(r, c) == std::optional<TouchTarget>{releasedTarget}) {
                                        if (const auto* released = deckState.getButton(r, c);
                                            released && released->hasDoublePressFlow) {
                                            pendingPress = PendingPress{releasedTarget, subIdx,
                                                                        nowTick + config.double_tap_ms};
                                        } else {
                                            fireShortPress(r, c, subIdx);
                                        }
                                    }
                                }
                            }
                        }

                        activeTouchRow = -1;
                        activeTouchCol = -1;
                        isDraggingSlider = false;
                        longPressTriggered = false;
                    }
                    break;
                }
            }
        }

        if (activeTouchTarget) {
            bool fingerStillDown = false;
            for (int i = 0; i < SDL_GetNumTouchFingers(activeTouchDevice); ++i) {
                const SDL_Finger* finger = SDL_GetTouchFinger(activeTouchDevice, i);
                if (finger && finger->id == activeFinger) {
                    fingerStillDown = true;
                    break;
                }
            }
            if (!fingerStillDown) cancelTouch();
        }

        if (auto error = ws.takeActionError(); !error.empty()) {
            const bool english = config.language == 1;
            if (error == "disconnected") {
                actionError = english ? "Macro Deck is disconnected" : "Macro Deck baglantisi kapali";
            } else if (error == "deck-sync") {
                actionError = english ? "Could not load the Macro Deck profile" : "Macro Deck profili yuklenemedi";
            } else if (error == "widget-rejected") {
                actionError = english ? "Macro Deck rejected the widget command" : "Macro Deck widget komutunu reddetti";
            } else if (error == "widget-reconnecting") {
                actionError = english ? "Widget is reconnecting" : "Widget yeniden bağlanıyor";
            } else if (error == "host-locked") {
                actionError = english ? "Macro Deck blocks controls while the computer is locked"
                                      : "Bilgisayar kilitliyken Macro Deck komutları engelliyor";
            } else if (error == "unconfirmed") {
                actionError = english ? "Macro execution could not be confirmed" : "Makronun calistigi dogrulanamadi";
            } else {
                actionError = (english ? "Macro Deck rejected the request: " : "Macro Deck istegi reddetti: ") + error;
            }
            actionErrorUntil = nowTick + 3000;
        } else if (!actionError.empty() && SDL_TICKS_PASSED(nowTick, actionErrorUntil)) {
            actionError.clear();
        }

#ifdef __SWITCH__
        if (running && config.idle_enabled && config.idle_mode == IdleScreenMode::Sleep &&
            !idleSleepAttempted && !activeTouchTarget && !ui.getSettingsUI().isOpen() && !showUpdateModal &&
            appletGetFocusState() == AppletFocusState_InFocus && nowTick - lastActivityTick >= idleTimeout) {
            idleSleepAttempted = true;
            if (R_FAILED(requestConsoleSleep())) {
                actionError = config.language == 1 ? "Switch could not enter sleep mode" : "Switch uyku moduna geçemedi";
                actionErrorUntil = nowTick + 4000;
            }
        }
#endif

        const Uint32 renderInterval = dimAlpha == 0 ? 33 : 100;
        if (dimAlpha != lastRenderedDim || (dimAlpha < 255 && nowTick - lastRenderTick >= renderInterval)) {
            if (dimAlpha < 255) {
                const float delta = static_cast<float>(nowTick - lastRenderTick) / 1000.0f;
                deckState.updateAnimations(nowTick, std::clamp(delta, 0.001f, 0.1f));
            }
            lastRenderedDim = dimAlpha;
            lastRenderTick = nowTick;
            ui.render(config, deckState, ws.getState(), ws.getPingMs(), ws.getStatusMessage(),
                      ws.isDiscovering(), ws.getDiscoveryStatus(), batteryPct, isCharging,
                      dimAlpha, showUpdateModal, updateModalVer, actionError);
        }

        const Uint32 loopInterval = dimAlpha == 255 ? 16 : 5;
        const Uint32 elapsed = SDL_GetTicks() - nowTick;
        if (elapsed < loopInterval) SDL_Delay(loopInterval - elapsed);
    }

    ws.stopAutoDiscovery();
    ws.disconnect();
    AppUpdater::cleanup();
    deckState.reset(ui.getRenderer());
    ui.shutdown();

#ifdef __SWITCH__
    appletUnhook(&resumeHook);
    lblExit();
    appletSetAutoSleepDisabled(false);
    appletSetMediaPlaybackState(false);
    psmExit();
    socketExit();
    csrngExit();
    nifmExit();
    romfsExit();
#endif

    return 0;
}
