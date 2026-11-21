from pathlib import Path
root=Path(__file__).resolve().parents[1]
state=(root/'source/deck_state.cpp').read_text(encoding='utf-8')
tree=(root/'source/widget_tree.cpp').read_text(encoding='utf-8')
ui=(root/'source/ui_renderer.cpp').read_text(encoding='utf-8')
ws=(root/'source/ws_client.cpp').read_text(encoding='utf-8')
updater=(root/'source/updater.cpp').read_text(encoding='utf-8')
def function(src,signature):
    a=src.index(signature)
    return src[a:src.index('\n}',a)+2]+'\n'
parts=['''#include "deck_state.hpp"
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
''']
for sig in ['static std::string trimText','double DeckButton::getCurrentProgressMs','float DeckButton::getCurrentProgressFraction','std::string DeckButton::formatDurationMs','bool DeckButton::countdownIsIdle','std::string DeckButton::countdownReadout','static std::string leafId','static std::string resolveWidgetText','std::string widgetNodeText','std::string widgetIconName','static void fillWeatherDetails','const nlohmann::json* selectResponsiveChild']:
    # The responsive helper's declaration can use the Json alias in the source.
    if sig not in state and 'selectResponsiveChild' in sig:
        import re
        m=re.search(r'^.*selectResponsiveChild\([^;]+\{',state,re.M)
        sig=m.group(0).split('{')[0].rstrip()
    parts.append(function(state,sig))
parts.append(function(tree,'std::string formatClockDate'))
for sig in ['void UIRenderer::drawText','void UIRenderer::drawWrappedText','void UIRenderer::drawVectorIcon']:
    parts.append(function(ui,sig).replace('UIRenderer::','FakeUi::'))
parts.append(function(state,'static void copyRuntimeState'))
parts.append(function(updater,'static std::optional<std::array<int, 3>> parseReleaseVersion'))
parts.append('''
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
''')
parts.append(function(ws,'bool WebSocketClient::readFrame').replace('WebSocketClient::','ConnectionTest::')
             .replace('timeval ', 'TestTimeout ').replace('recv(', 'readSocket('))
parts.append('''
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
''')
for sig in ['void WebSocketClient::syncWidgetSessions', 'void WebSocketClient::queueSessionAttach',
            'void WebSocketClient::queueSessionClose', 'void WebSocketClient::retryWidgetSession',
            'void WebSocketClient::resyncWidgetSession', 'bool WebSocketClient::hasWidgetSession',
            'bool WebSocketClient::handleUiSessionMessage', 'void WebSocketClient::sendUiEvent']:
    parts.append(function(ws,sig).replace('WebSocketClient::','WidgetConnectionTest::'))
parts.append('''
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
''')
for sig in ['std::string widgetArtworkId', 'std::string widgetArtworkKey']:
    parts.append(function(state,sig))
parts.append('''
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
''')
artwork = tree.index('    void artwork(')
parts.append(tree[artwork:tree.index('\n    }', artwork)+6].replace('void artwork(', 'void ArtworkTest::artwork(', 1))
parts.append('''
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
''')
parts.append('''
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
''')
parts.append(function(state,'static void pauseInvalidatedMusic'))
parts.insert(1, 'Uint64 SDL_GetTicks64() { return 1000; }\n')
for sig in ['bool DeckState::handleMd3WidgetUpsert','const DeckButton* DeckState::findSpotifyPlayAction',
            'void DeckState::registerWidgetSession','void DeckState::handleUiSessionClosedOrInvalidated']:
    parts.append(function(state,sig).replace('DeckState::','SessionState::'))
for sig in ['const Json& object','const Json& children','int alignedOffset','SDL_Rect inset','bool textComponent','bool levelComponent']:
    parts.append(function(tree,sig))
# Compile the production layout engine with deterministic font metrics; no GPU or Switch is emulated.
a=tree.index('struct UIRenderer::WidgetTree {')
b=tree.index('    SDL_Color accent()',a)
layout=tree[a:b].replace('struct UIRenderer::WidgetTree','struct Layout').replace('UIRenderer& ui','FakeUi& ui')+'};\n'
parts.append(layout)
parts.append(r'''
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
 alignas(DeckState) unsigned char stateStorage[sizeof(DeckState)]{};
 auto& mockState=*reinterpret_cast<DeckState*>(stateStorage);
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
''')
# Avoid forming an unconstructed DeckState: the layout reference is replaced with an empty test type.
parts=[p.replace('const DeckState& state;', 'const int& state;') for p in parts]
parts[-1]=parts[-1].replace('alignas(DeckState) unsigned char stateStorage[sizeof(DeckState)]{};\n auto& mockState=*reinterpret_cast<DeckState*>(stateStorage);','const int mockState=0;')
(root/'tests/generated.cpp').write_text('\n'.join(parts),encoding='utf-8')
print('Generated tests from production functions')
