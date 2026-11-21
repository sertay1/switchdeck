#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <SDL2/SDL.h>
#include <json.hpp>

// Null-safe JSON helper functions (nlohmann::json::value throws type_error.302 when a key exists with value null)
inline std::string jsonStr(const nlohmann::json& j, const char* key, const std::string& def = "") {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer() || it->is_number_unsigned()) return std::to_string(it->get<long long>());
    return def;
}

inline int jsonInt(const nlohmann::json& j, const char* key, int def = 0) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<int>();
    return def;
}

inline double jsonDouble(const nlohmann::json& j, const char* key, double def = 0.0) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<double>();
    return def;
}

inline bool jsonBool(const nlohmann::json& j, const char* key, bool def = false) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    return def;
}

struct SubButtonNode {
    std::string nodeId;
    std::string iconName;
    std::string label;
};

struct WeatherForecastDay {
    std::string label;
    std::string minTemp;
    std::string maxTemp;
    std::string resourceId;
};

struct GaugeItem {
    std::string name;
    std::string reading;
    float level = 0.0f;
    uint8_t r = 33;
    uint8_t g = 150;
    uint8_t b = 243;
    bool hasColor = false;
    std::string resourceId;
};

struct BatteryDeviceItem {
    std::string name;
    std::string valueText;
    std::string stateText;
    std::string iconName;
    float level = 0.0f;
    bool isCharging = false;
    uint8_t r = 34;
    uint8_t g = 197;
    uint8_t b = 94;
    bool hasColor = false;
    std::string resourceId;
};

struct CalendarEventItem {
    std::string title;
    std::string timeText;
    std::string location;
};

struct DeckButton {
    std::string widgetId;
    std::string folderId;
    std::string widgetType = "ActionButton"; // ActionButton, MusicPlayer, Slider, Clock, Weather, etc.

    int posX = 0;
    int posY = 0;
    int width = 1;
    int height = 1;

    std::string iconBase64;
    std::string labelBase64;

    // Static & Live Text
    std::string staticLabel;
    std::string staticSubLabel;
    std::string labelText;
    std::string subLabelText;
    std::string thirdLabelText;

    // ActionButton layout & typography
    std::string labelPosition = "center"; // "top", "center", "bottom"
    std::string textAlign = "center";     // "left", "center", "right"
    int fontSize = 14;
    bool labelShadow = true;
    std::string iconFit = "contain";      // "contain", "cover"
    int iconZoom = 100;                   // 50..200 (%)
    int iconOffsetX = 0;                  // -50..50 (%)
    int iconOffsetY = 0;                  // -50..50 (%)
    int iconOpacity = 100;                // 0..100 (%)

    // Colors (Macro Deck 3 defaults: --color-bg-secondary #1a1a1a, --color-accent #2196f3)
    uint8_t bgR = 26;
    uint8_t bgG = 26;
    uint8_t bgB = 26;
    bool hasBgColor = false;

    uint8_t accentR = 33;
    uint8_t accentG = 150;
    uint8_t accentB = 243;
    bool hasAccentColor = false;

    uint8_t textR = 255;
    uint8_t textG = 255;
    uint8_t textB = 255;
    bool hasTextColor = false;

    uint8_t borderR = 51;
    uint8_t borderG = 51;
    uint8_t borderB = 51;
    bool hasBorder = false;
    bool borderExplicitlyOff = false;
    std::string borderStyle;

    // Icons & Artwork
    std::string iconName;
    std::string resourceId;
    std::string loadedResourceId;
    std::vector<std::string> treeResourceIds;

    // MusicPlayer specific state (matching MusicPlayerWidgetData & MusicPlayerViewState)
    std::string musicCoverStyle = "small"; // "small" or "full"
    bool musicShowHeader = true;
    bool musicShowTitle = true;
    bool musicShowArtist = true;
    bool musicShowAlbum = true;
    bool musicShowTimeline = true;
    bool musicShowSource = true;

    std::string musicProviderName = "Spotify";
    std::string musicSourceName;
    std::string musicBadgeText;
    std::string musicTitle;
    std::string musicArtist;
    std::string musicAlbum;
    bool musicIsPlaying = false;
    bool musicIsPaused = false;
    bool musicHasWarning = false;
    bool musicHasTrack = false;
    std::string musicArtworkId;
    std::string musicArtworkKey;

    // Live $progress interpolation (positionMs + (nowTicks - anchorTicks) * rate)
    double progressPositionMs = 0.0;
    double progressDurationMs = 0.0;
    double progressRate = 0.0;
    std::string progressAnchorStr;
    bool treeHasProgress = false;
    double progressAnchorOffsetMs = 0;
    Uint64 progressAnchorTicks = 0;

    // Slider / RangeBar state
    bool sliderIsVertical = false;
    bool sliderIsRelative = false;
    bool sliderShowLabel = true;
    bool sliderShowValue = false;
    std::string sliderValueText;
    std::string sliderValueVariable;
    float sliderLevel = 0.0f; // 0.0 .. 1.0
    double sliderMin = 0.0;
    double sliderMax = 100.0;
    double sliderStep = 1.0;
    Uint32 lastLocalSlideTick = 0;
    bool hasProgressBar = false;
    bool hasShortPressFlow = false;
    bool spotifyPlayAction = false;
    std::string musicInstanceId;
    bool hasDoublePressFlow = false;
    bool hasLongPressFlow = false;

    // Clock specific state
    bool clockIsAnalog = false;
    bool clockConfiguredAnalog = false;
    bool clockShowSeconds = true;
    bool clockShowDate = true;
    std::string clockDatePosition = "below"; // "above" or "below"
    std::string clockTimeText;
    std::string clockDateText;
    std::string clockZoneText;

    // Weather specific state
    std::string weatherStyle = "compact"; // "compact" or "forecast"
    std::string weatherTemp;
    std::string weatherCondition;
    std::string weatherLocation;
    std::string weatherHighLow;
    std::string weatherHigh;
    std::string weatherLow;
    std::vector<WeatherForecastDay> weatherForecast;

    // Gauges specific state
    bool gaugeIsArc = false;
    std::vector<GaugeItem> gauges;

    // Countdown specific state
    float countdownRingLevel = 1.0f;
    std::string countdownTimeText;
    double countdownConfiguredMs = 0;
    std::string countdownCaption;
    bool countdownIsFinished = false;
    bool countdownIsPaused = false;
    bool countdownIsRunning = false;

    // Battery specific state
    float batteryLevel = -1.0f;
    bool batteryIsCharging = false;
    std::string batteryText;
    std::string batteryStateText;
    std::vector<BatteryDeviceItem> batteryDevices;

    // Stopwatch specific state
    std::string stopwatchTimeText;
    bool stopwatchIsRunning = false;
    bool stopwatchIsPaused = false;

    // Calendar specific state
    std::string calendarMonthText;
    std::string calendarDayText;
    std::string calendarWeekdayText;
    std::string calendarAgendaHeading;
    std::vector<CalendarEventItem> calendarEvents;
    std::string calendarNextDate;
    std::string calendarNextTitle;
    std::string calendarNextWhen;
    std::string calendarNextLocation;

    // HistoryGraph specific state
    std::string graphTitle;
    std::string graphSubtitle;
    std::string graphUnit;
    std::vector<float> graphHistory;

    // Local Folder Navigation (if configured in flows)
    std::string navCommand;        // "changeTo", "parent", "back"
    std::string navTargetFolderId;

    // Macro Deck 3 UI Session state
    std::string sessionId;
    int revision = 0;
    nlohmann::json uiTree;
    std::string primaryNodeId;
    std::vector<std::string> primaryNodeEvents;
    std::string sliderNodeId;
    std::string activeLevelNode;
    std::vector<SubButtonNode> subButtons;

    bool isPressed = false;
    std::string pressedUiNode;
    int activeSubBtnIndex = -1;

    // Macro Deck 3 animation state
    float pressFeedbackSeconds = 0.0f;
    float visualPressFraction = 0.0f; // 0.0 .. 1.0 (asymmetric: 20ms ease-in, 140ms ease-out)
    float visualSliderLevel = -1.0f;  // 0.0 .. 1.0 (smooth 80ms linear transition)
    Uint64 lastAnimUpdateTick = 0;

    void updateAnimations(Uint64 nowTicks, float deltaSec);

    SDL_Texture* iconTexture = nullptr;
    SDL_Texture* labelTexture = nullptr;
    int iconWidth = 0;
    int iconHeight = 0;
    int labelWidth = 0;
    int labelHeight = 0;

    void cleanupTextures();
    double getCurrentProgressMs(Uint64 nowTicks, bool clampToDuration = true) const;
    float getCurrentProgressFraction(Uint64 nowTicks) const;
    bool countdownIsIdle() const;
    std::string countdownReadout(Uint64 nowTicks) const;
    static std::string formatDurationMs(double ms, bool roundUp = false);
};

struct DeckFolder {
    std::string id;
    std::string profileId;
    std::string name;
    std::string parentId;
    bool isDefault = false;
    int rows = -1;
    int cols = -1;
    int widgetSpacing = -1;
    int widgetBorderRadius = -1;
    std::string backgroundColor;
    bool hasBgColor = false;
    uint8_t bgR = 18;
    uint8_t bgG = 18;
    uint8_t bgB = 18;
    std::vector<DeckButton> widgets;
};

struct ProfileInfo {
    std::string id;
    std::string name;
    bool isDefault = false;
};

struct FolderSummary {
    std::string id;
    std::string name;
    bool isDefault;
};

class DeckState {
public:
    DeckState();
    ~DeckState();

    void reset(SDL_Renderer* renderer);

    // Macro Deck 2 Legacy handlers
    void handleConfig(const nlohmann::json& j);
    void handleButtons(const nlohmann::json& j, SDL_Renderer* renderer);
    void handleUpdateButton(const nlohmann::json& j, SDL_Renderer* renderer);
    void handleUpdateLabel(const nlohmann::json& j, SDL_Renderer* renderer);

    // Macro Deck 3 REST & Event handlers
    std::string handleMd3Profiles(const nlohmann::json& j, const std::string& preferredProfileId);
    void handleMd3ProfileUpdate(const nlohmann::json& pJson, SDL_Renderer* renderer = nullptr);
    void handleMd3Folders(const nlohmann::json& j, const std::string& profileId,
                          bool profileChanged, SDL_Renderer* renderer);
    void handleMd3FolderUpsert(const nlohmann::json& folderJson, SDL_Renderer* renderer);
    void handleMd3FolderDelete(const std::string& folderId, SDL_Renderer* renderer);
    bool handleMd3WidgetUpsert(const std::string& folderId, const nlohmann::json& widgetJson, SDL_Renderer* renderer);
    void handleMd3WidgetDelete(const std::string& folderId, const std::string& widgetId, SDL_Renderer* renderer);
    void handleWidgetsDeleted(const std::string& folderId, const std::vector<std::string>& widgetIds, SDL_Renderer* renderer);
    void handleWidgetStateInvalidated(const std::string& widgetId);
    void handleUiSessionClosedOrInvalidated(const std::string& sessionId);

    // Macro Deck 3 Live Widget UI Session handlers
    void registerWidgetSession(const std::string& widgetId, const std::string& sessionId);
    void handleUiSessionTreeUpdated(const std::string& sessionId, int revision, const nlohmann::json& tree);
    bool handleUiSessionPatched(const std::string& sessionId, const nlohmann::json& patch);
    void setWidgetImageBytes(const std::string& widgetId, const std::string& resourceId, const std::string& rawBytes, SDL_Renderer* renderer);
    std::vector<std::pair<std::string, std::string>> getPendingResourceRequests() const;
    SDL_Texture* getResourceTexture(const std::string& resourceId, int* outW = nullptr, int* outH = nullptr, Uint64 now = 0) const;

    // Folder & Page Navigation
    void setPreferredInitialFolder(const std::string& folderId) { preferredInitialFolderId_ = folderId; }
    bool openFolder(const std::string& folderId, SDL_Renderer* renderer);
    bool goParentFolder(SDL_Renderer* renderer);
    bool goBackFolder(SDL_Renderer* renderer);
    bool goHomeFolder(SDL_Renderer* renderer);
    bool nextFolder(SDL_Renderer* renderer);
    bool prevFolder(SDL_Renderer* renderer);
    bool canGoBackOrParent() const;
    int getCurrentFolderIndex() const;
    int getFolderCount() const;
    std::string getCurrentFolderId() const { return currentFolderId_; }
    std::string getCurrentFolderName() const;
    bool getCurrentFolderBg(uint8_t& r, uint8_t& g, uint8_t& b) const;
    std::string getCurrentProfileId() const { return currentProfileId_; }
    std::string getCurrentProfileName() const { return currentProfileName_; }
    const std::vector<ProfileInfo>& getProfiles() const { return profiles_; }
    bool switchProfile(const std::string& profileId, SDL_Renderer* renderer);
    const std::vector<DeckFolder>& getFolders() const { return folders_; }
    std::vector<FolderSummary> getActiveProfileFolders() const;

    DeckButton* getButton(int y, int x);
    const DeckButton* getButtonConst(int y, int x) const;
    DeckButton* getButtonById(const std::string& widgetId);
    std::vector<DeckButton>& getButtons() { return buttons_; }
    const std::vector<DeckButton>& getButtons() const { return buttons_; }
    const DeckButton* findSpotifyPlayAction(const DeckButton& player) const;
    std::vector<std::string> getDisplayedWidgetIds() const;

    int getRows() const { return rows_; }
    int getCols() const { return cols_; }
    int getSpacing() const { return spacing_; }
    int getRadius() const { return radius_; }
    bool isEmptyCellsVisible() const { return emptyCellsVisible_; }
    void updateFolderLayoutLocally(const std::string& folderId, int rows, int cols, int spacing, int radius, SDL_Renderer* renderer = nullptr);

    void setButtonState(int y, int x, bool pressed, int subBtnIdx = -1);
    void updateAnimations(Uint64 nowTicks, float deltaSec);

    static SDL_Texture* createTextureFromBase64(const std::string& b64, SDL_Renderer* renderer, int& outW, int& outH, bool roundCorners = false);
    static SDL_Texture* createTextureFromBytes(const uint8_t* data, size_t len, SDL_Renderer* renderer, int& outW, int& outH, bool roundCorners = false);

private:
    void updateSingleButton(const nlohmann::json& bJson, SDL_Renderer* renderer);
    DeckButton parseMd3Widget(const nlohmann::json& wJson, const std::string& folderId);
    void syncActiveFolderToButtons(SDL_Renderer* renderer);
    void extractStateFromUiTree(DeckButton& btn);
    void bindResourceIcon(DeckButton& btn);
    void bindButtonIcon(DeckButton& btn, SDL_Renderer* renderer);
    void removeCachedResource(const std::string& key);
    struct ArtworkFrame {
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture;
        Uint64 endsAt;
    };
    bool cacheResource(const std::string& key, SDL_Texture* texture, int width, int height, size_t hash = 0,
                       std::vector<ArtworkFrame> frames = {}, uint32_t loops = 0);
    bool cacheAnimatedWebP(const std::string& id, const std::string& bytes, SDL_Renderer* renderer);
    bool cacheAnimatedGif(const std::string& id, const std::string& bytes, SDL_Renderer* renderer);

    int rows_ = 3;
    int cols_ = 6;
    int spacing_ = 16;
    int radius_ = 28;
    int profileDefaultRows_ = 3;
    int profileDefaultCols_ = 6;
    int profileDefaultSpacing_ = 16;
    int profileDefaultRadius_ = 28;
    bool emptyCellsVisible_ = true;

    std::string currentProfileId_;
    std::string currentProfileName_ = "Default";
    std::string currentFolderId_;
    bool currentFolderHasBgColor_ = false;
    uint8_t currentFolderBgR_ = 18;
    uint8_t currentFolderBgG_ = 18;
    uint8_t currentFolderBgB_ = 18;
    std::string preferredInitialFolderId_;
    std::vector<std::string> folderHistory_;
    std::vector<ProfileInfo> profiles_;
    std::vector<DeckFolder> folders_;
    std::map<std::string, std::string> sessionToWidget_;

    struct CachedResource {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
        size_t contentHash = 0;
        size_t bytes = 0;
        uint64_t insertionOrder = 0;
        std::vector<ArtworkFrame> frames;
        Uint64 animationStart = 0;
        uint32_t loops = 0;
    };
    std::map<std::string, CachedResource> resourceCache_;
    size_t resourceCacheBytes_ = 0;
    uint64_t nextResourceOrder_ = 0;

    std::vector<DeckButton> buttons_;
};

const nlohmann::json* selectResponsiveChild(const nlohmann::json& node, double widthCells, double heightCells);

std::string widgetIconName(const nlohmann::json& properties);
std::string widgetNodeText(const nlohmann::json& node, const nlohmann::json& properties);
std::string widgetArtworkId(const nlohmann::json& properties);
std::string widgetArtworkKey(const nlohmann::json& properties);
