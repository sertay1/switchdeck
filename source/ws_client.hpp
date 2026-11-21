#pragma once
#include <string>
#include <vector>
#include <set>
#include <map>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <chrono>
#include <array>
#include <condition_variable>
#include <json.hpp>
#include "config.hpp"

enum class WsState {
    Disconnected,
    Connecting,
    Connected
};

struct ResourceImageResult {
    std::string widgetId;
    std::string resourceId;
    std::string rawBytes;
};

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string&)>;
    using StateCallback = std::function<void(WsState, const std::string& errorMsg)>;
    using ResourceCallback = std::function<void(const ResourceImageResult&)>;

    WebSocketClient();
    ~WebSocketClient();

    void setCallbacks(MessageCallback onMsg, StateCallback onState, ResourceCallback onResource = nullptr);
    void connect(const AppConfig& config);
    void disconnect();
    bool isConnected() const;
    WsState getState() const;
    std::string getStatusMessage() const;
    AppConfig getCurrentConfig() const;
    void updateConfig(const AppConfig& config);
    void resetSession();
    std::string takeActionError();
    int getPingMs() const { return pingMs_.load(); }

    // Auto-Discovery (UDP Broadcast + Fast /24 Subnet Probe)
    void startAutoDiscovery(int port = 8193);
    void stopAutoDiscovery();
    bool isDiscovering() const { return discovering_.load(); }
    std::string getDiscoveryStatus() const;
    bool consumeDiscoveredServer(std::string& outHost, int& outPort);

    void sendText(const std::string& text);

    // Macro Deck 3 Higher-Level Methods
    std::vector<std::string> syncFolderAndWidgets(const std::string& folderId, const std::vector<std::string>& widgetIds);
    void resyncWidgetSession(const std::string& sessionId);
    void invalidateWidget(const std::string& widgetId);
    bool hasWidgetSession(const std::string& widgetId);
    void sendUiEvent(const std::string& sessionId, const std::string& nodeId, const std::string& eventName, const nlohmann::json& data, int revision);
    void executeAction(const std::string& widgetId, const std::string& folderId, const std::string& triggerType);
    void requestResourceImage(const std::string& widgetId, const std::string& resourceId, bool musicCover = false);
    void requestDeckReload();
    void requestAppearanceFonts();
    void updateFolderLayout(const std::string& folderId, int rows, int cols, int spacing, int radius);

    void poll();

private:
    struct WidgetSessionAttempt {
        std::string requestId;
        std::chrono::steady_clock::time_point deadline;
    };

    // These helpers run with queueMutex_ held.
    void syncWidgetSessions();
    void queueSessionAttach(const std::string& widgetId, const std::string& sessionId);
    void queueSessionClose(const std::string& sessionId);
    void retryWidgetSession(const std::string& widgetId, const std::string& sessionId);
    bool handleUiSessionMessage(const nlohmann::json& envelope);

    struct HttpTask {
        enum class Type {
            FetchResource,
            ReloadDeck,
            AppearanceFonts,
            UpdateFolderLayout
        } type;
        std::string arg1;
        std::string arg2;
        bool musicCover = false;
        int intVal1 = 0;
        int intVal2 = 0;
        int intVal3 = 0;
        int intVal4 = 0;
    };

    struct ActionRequest {
        std::string widgetId;
        std::string folderId;
        std::string triggerType;
        uint64_t connection;
    };

    void workerThread();
    void httpWorker();
    void actionWorker();
    void stopWorkers();
    bool waitToReconnect(std::chrono::seconds delay);
    void reportActionError(const std::string& message);
    void discoveryWorker(int targetPort);
    void setStatus(WsState st, const std::string& msg);
    enum class RefreshResult { Ready, Rejected, Unavailable };
    RefreshResult tryRefreshToken(const std::string& rejectedToken = {});
    bool fetchDeckFromServer();
    void fetchAppearanceFonts();

    bool httpRequest(const std::string& method, const std::string& host, int port, const std::string& path,
                     bool useAuth, const std::string& extraHeaders, const std::string& body,
                     int& outStatusCode, std::string& outBody);

    bool doHandshake(int sock, const std::string& host, int port, const std::string& path, const std::string& subprotocol);
    bool sendFrame(int sock, uint8_t opcode, const uint8_t* payload, size_t length);
    bool readFrame(int sock, std::string& outMessage, int headerTimeoutMs = 25);

    mutable std::mutex configMutex_;
    std::mutex refreshMutex_;
    AppConfig config_;

    std::atomic<bool> running_{false};
    std::atomic<WsState> state_{WsState::Disconnected};
    std::atomic<int> pingMs_{-1};
    std::chrono::steady_clock::time_point lastPingSentTime_;
    uint64_t pingSequence_ = 0;
    bool awaitingPong_ = false;
    std::array<int, 3> pingSamples_{};
    size_t nextPingSample_ = 0;

    // Discovery state
    std::atomic<bool> discovering_{false};
    std::atomic<bool> stopDiscovery_{false};
    std::atomic<bool> hasDiscoveredServer_{false};
    mutable std::mutex discoveryMutex_;
    std::string discoveryStatus_;
    std::string discoveredHost_;
    int discoveredPort_ = 8193;
    std::thread discoveryThread_;

    mutable std::mutex statusMutex_;
    std::string statusMsg_ = "Bagli Degil";
    std::string actionError_;
    std::mutex socketMutex_;
    int socket_ = -1;
    std::atomic<uint64_t> reqSeq_{1};

    std::thread thread_;
    std::thread httpThread_;
    std::thread actionThread_;
    std::mutex httpMutex_;
    std::condition_variable httpCv_;
    std::queue<HttpTask> httpTaskQueue_;
    std::queue<ActionRequest> actionQueue_;
    uint64_t connection_ = 0;

    std::mutex queueMutex_;
    std::queue<std::string> incomingQueue_;
    std::queue<std::string> outgoingQueue_;
    std::queue<ResourceImageResult> resourceResultQueue_;
    std::map<std::string, WidgetSessionAttempt> widgetSessionAttempts_;
    std::set<std::string> displayedWidgets_;
    std::string reportedFolder_;
    std::map<std::string, std::string> sessionToWidget_;
    std::set<std::string> requestedResources_;
    std::map<std::string, std::chrono::steady_clock::time_point> resourceRetryAfter_;

    MessageCallback onMessage_;
    StateCallback onState_;
    ResourceCallback onResource_;
};
