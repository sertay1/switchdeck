#include "ws_client.hpp"
#include "deck_state.hpp"
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <random>
#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

static std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (unsigned char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << std::uppercase;
            escaped << '%' << std::setw(2) << int(c);
            escaped << std::nouppercase;
        }
    }
    return escaped.str();
}

static std::string decodeChunkedBody(const std::string& raw) {
    std::string out;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t lineEnd = raw.find("\r\n", pos);
        if (lineEnd == std::string::npos) break;
        std::string hexLen = raw.substr(pos, lineEnd - pos);
        size_t semi = hexLen.find(';');
        if (semi != std::string::npos) hexLen = hexLen.substr(0, semi);
        size_t chunkLen = 0;
        try {
            chunkLen = std::stoul(hexLen, nullptr, 16);
        } catch (...) {
            return raw;
        }
        pos = lineEnd + 2;
        if (chunkLen == 0) break;
        if (pos + chunkLen > raw.size()) {
            out.append(raw.data() + pos, raw.size() - pos);
            break;
        }
        out.append(raw.data() + pos, chunkLen);
        pos += chunkLen;
        if (pos + 2 <= raw.size() && raw[pos] == '\r' && raw[pos + 1] == '\n') {
            pos += 2;
        }
    }
    return out;
}

static std::string cleanJsonString(const std::string& s) {
    size_t firstBrace = s.find('{');
    size_t firstBracket = s.find('[');
    size_t start = std::string::npos;
    char closeChar = '}';
    if (firstBrace != std::string::npos && (firstBracket == std::string::npos || firstBrace < firstBracket)) {
        start = firstBrace;
        closeChar = '}';
    } else if (firstBracket != std::string::npos) {
        start = firstBracket;
        closeChar = ']';
    }
    if (start == std::string::npos) return s;
    size_t end = s.rfind(closeChar);
    if (end != std::string::npos && end >= start) {
        return s.substr(start, end - start + 1);
    }
    return s.substr(start);
}

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() {
    stopAutoDiscovery();
    disconnect();
}

void WebSocketClient::setCallbacks(MessageCallback onMsg, StateCallback onState, ResourceCallback onResource) {
    onMessage_ = onMsg;
    onState_ = onState;
    onResource_ = onResource;
}

void WebSocketClient::setStatus(WsState st, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lock(httpMutex_);
        if (st != WsState::Connected) {
            ++connection_;
            actionQueue_ = {};
        }
        state_ = st;
    }
    if (st != WsState::Connected) {
        pingMs_ = -1;
    }
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        statusMsg_ = msg;
    }
    nlohmann::json ev;
    ev["_internal"] = "WS_STATE";
    ev["state"] = static_cast<int>(st);
    ev["msg"] = msg;
    std::lock_guard<std::mutex> qlock(queueMutex_);
    incomingQueue_.push(ev.dump());
}

std::string WebSocketClient::getStatusMessage() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return statusMsg_;
}

AppConfig WebSocketClient::getCurrentConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

void WebSocketClient::updateConfig(const AppConfig& config) {
    std::scoped_lock lock(httpMutex_, configMutex_);
    AppConfig settings = config;
    settings.host = config_.host;
    settings.port = config_.port;
    settings.pairing_code = config_.pairing_code;
    settings.copySessionFrom(config_);
    config_ = std::move(settings);
    ConfigManager::save(config_);
    httpCv_.notify_all();
}

void WebSocketClient::resetSession() {
    disconnect();
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_.copySessionFrom(AppConfig{});
        config_.pairing_code.clear();
        ConfigManager::save(config_);
    }
    setStatus(WsState::Disconnected, "Eslestirme gerekli: Ayarlar'dan yeni kod girin");
}

void WebSocketClient::connect(const AppConfig& config) {
    disconnect();
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
        ConfigManager::save(config_);
    }
    running_ = true;
    pingMs_ = -1;
    setStatus(WsState::Connecting, "Baglaniyor...");
    thread_ = std::thread(&WebSocketClient::workerThread, this);
    httpThread_ = std::thread(&WebSocketClient::httpWorker, this);
    actionThread_ = std::thread(&WebSocketClient::actionWorker, this);
}

void WebSocketClient::stopWorkers() {
    {
        std::lock_guard<std::mutex> lock(httpMutex_);
        running_ = false;
        ++connection_;
        actionQueue_ = {};
    }
    httpCv_.notify_all();
}

bool WebSocketClient::waitToReconnect(std::chrono::seconds delay) {
    std::unique_lock<std::mutex> lock(httpMutex_);
    const auto shouldStop = [this]() { return !running_ || !getCurrentConfig().auto_reconnect; };
    return !httpCv_.wait_for(lock, delay, shouldStop);
}

void WebSocketClient::disconnect() {
    stopWorkers();
    pingMs_ = -1;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (socket_ >= 0) shutdown(socket_, SHUT_RDWR);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (httpThread_.joinable()) {
        httpThread_.join();
    }
    if (actionThread_.joinable()) {
        actionThread_.join();
    }
    {
        std::lock_guard<std::mutex> qlock(queueMutex_);
        incomingQueue_ = {};
        outgoingQueue_ = {};
        resourceResultQueue_ = {};
        widgetSessionAttempts_.clear();
        displayedWidgets_.clear();
        reportedFolder_.clear();
        sessionToWidget_.clear();
        requestedResources_.clear();
        resourceRetryAfter_.clear();
    }
    {
        std::lock_guard<std::mutex> hlock(httpMutex_);
        httpTaskQueue_ = {};
    }
    state_ = WsState::Disconnected;
}

void WebSocketClient::reportActionError(const std::string& message) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    actionError_ = message;
}

std::string WebSocketClient::takeActionError() {
    std::lock_guard<std::mutex> lock(statusMutex_);
    std::string result;
    actionError_.swap(result);
    return result;
}

bool WebSocketClient::isConnected() const {
    return running_ && state_ == WsState::Connected;
}

WsState WebSocketClient::getState() const {
    return state_;
}

// ============================================================================
// Auto-Discovery (UDP Broadcast + Fast Non-Blocking /24 Subnet Probe)
// ============================================================================

static std::string detectLocalSubnetPrefix(const std::string& fallbackHost) {
    int usock = socket(AF_INET, SOCK_DGRAM, 0);
    if (usock >= 0) {
        struct sockaddr_in remote;
        memset(&remote, 0, sizeof(remote));
        remote.sin_family = AF_INET;
        remote.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
        if (::connect(usock, (struct sockaddr*)&remote, sizeof(remote)) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            if (getsockname(usock, (struct sockaddr*)&local, &len) == 0) {
                char buf[INET_ADDRSTRLEN] = {0};
                if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
                    std::string ip(buf);
                    size_t lastDot = ip.rfind('.');
                    if (lastDot != std::string::npos && ip.rfind("127.", 0) != 0 && ip != "0.0.0.0") {
                        close(usock);
                        return ip.substr(0, lastDot + 1);
                    }
                }
            }
        }
        close(usock);
    }

    size_t lastDot = fallbackHost.rfind('.');
    if (lastDot != std::string::npos) {
        return fallbackHost.substr(0, lastDot + 1);
    }
    return "192.168.1.";
}

static bool connectWithTimeout(int sock, const sockaddr_in& address, int timeoutMs) {
    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return false;

    int result = ::connect(sock, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result < 0 && errno == EINPROGRESS) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(sock, &writable);
        timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(sock + 1, nullptr, &writable, nullptr, &timeout) > 0) {
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) {
                result = 0;
            }
        }
    }
    return fcntl(sock, F_SETFL, flags) == 0 && result == 0;
}

static bool sendAll(int sock, const void* bytes, size_t length) {
    const auto* data = static_cast<const char*>(bytes);
    while (length > 0) {
        const auto sent = send(sock, data, length, 0);
        if (sent <= 0) return false;
        data += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

static std::string lowerAscii(std::string value) {
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

static std::string trimHttpWhitespace(const std::string& value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

static bool probeMacroDeckHttp(const std::string& ip, int port, int timeoutMs) {
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0 || !connectWithTimeout(sock, addr, timeoutMs)) {
        close(sock);
        return false;
    }

    timeval timeout{0, 350000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const std::string request = "GET / HTTP/1.0\r\nHost: " + ip + ":" + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    if (!sendAll(sock, request.data(), request.size())) {
        close(sock);
        return false;
    }

    // Identify the web client itself. An open port or an arbitrary HTTP reply is not enough.
    std::string response;
    char chunk[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    while (response.size() < 32 * 1024 && std::chrono::steady_clock::now() < deadline) {
        const auto received = recv(sock, chunk, sizeof(chunk), 0);
        if (received <= 0) break;
        response.append(chunk, static_cast<size_t>(received));
        if (lowerAscii(response).find("</title>") != std::string::npos) break;
    }
    close(sock);

    std::istringstream statusLine(response.substr(0, response.find("\r\n")));
    std::string version;
    int status = 0;
    if (!(statusLine >> version >> status) || (version != "HTTP/1.0" && version != "HTTP/1.1") || status != 200) return false;
    const auto headersEnd = response.find("\r\n\r\n");
    if (headersEnd == std::string::npos) return false;
    const auto html = lowerAscii(response.substr(headersEnd + 4));
    const auto titleStart = html.find("<title>");
    if (titleStart == std::string::npos) return false;
    const auto titleEnd = html.find("</title>", titleStart);
    if (titleEnd == std::string::npos) return false;
    const auto title = trimHttpWhitespace(html.substr(titleStart + 7, titleEnd - titleStart - 7));
    return title == "macro deck" || title == "macro deck 3" ||
           title == "macro deck :: web client" || title == "macro deck web client" || title == "macrodeck";
}

void WebSocketClient::startAutoDiscovery(int port) {
    stopAutoDiscovery();
    stopDiscovery_ = false;
    hasDiscoveredServer_ = false;
    discovering_ = true;
    {
        std::lock_guard<std::mutex> lock(discoveryMutex_);
        discoveryStatus_ = "Ag taraniyor...";
    }
    discoveryThread_ = std::thread(&WebSocketClient::discoveryWorker, this, port > 0 ? port : 8193);
}

void WebSocketClient::stopAutoDiscovery() {
    stopDiscovery_ = true;
    if (discoveryThread_.joinable()) {
        discoveryThread_.join();
    }
    discovering_ = false;
}

std::string WebSocketClient::getDiscoveryStatus() const {
    std::lock_guard<std::mutex> lock(discoveryMutex_);
    return discoveryStatus_;
}

bool WebSocketClient::consumeDiscoveredServer(std::string& outHost, int& outPort) {
    if (!hasDiscoveredServer_.load()) return false;
    std::lock_guard<std::mutex> lock(discoveryMutex_);
    outHost = discoveredHost_;
    outPort = discoveredPort_;
    hasDiscoveredServer_ = false;
    return true;
}

void WebSocketClient::discoveryWorker(int targetPort) {
    AppConfig cfg = getCurrentConfig();
    std::string prefix = detectLocalSubnetPrefix(cfg.host);

    // 1. Send UDP Broadcast Discovery packet on 8193 & 8191
    {
        int bsock = socket(AF_INET, SOCK_DGRAM, 0);
        if (bsock >= 0) {
            int broadcastEnable = 1;
            setsockopt(bsock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcastEnable, sizeof(broadcastEnable));
            struct sockaddr_in baddr;
            memset(&baddr, 0, sizeof(baddr));
            baddr.sin_family = AF_INET;
            baddr.sin_port = htons(targetPort);
            std::string bcastIp = prefix + "255";
            inet_pton(AF_INET, bcastIp.c_str(), &baddr.sin_addr);
            const char* probeMsg = "MACRODECK_DISCOVER";
            sendto(bsock, probeMsg, strlen(probeMsg), 0, (struct sockaddr*)&baddr, sizeof(baddr));
            close(bsock);
        }
    }

    // 2. Quick probe of currently configured host first
    if (!cfg.host.empty() && !stopDiscovery_) {
        {
            std::lock_guard<std::mutex> lock(discoveryMutex_);
            discoveryStatus_ = "Kontrol ediliyor: " + cfg.host + ":" + std::to_string(targetPort);
        }
        if (probeMacroDeckHttp(cfg.host, targetPort, 220)) {
            std::lock_guard<std::mutex> lock(discoveryMutex_);
            discoveredHost_ = cfg.host;
            discoveredPort_ = targetPort;
            discoveryStatus_ = "Bulundu: " + cfg.host + ":" + std::to_string(targetPort) + " (Baglan'i secin)";
            hasDiscoveredServer_ = true;
            discovering_ = false;
            return;
        }
    }

    // 3. Fast parallel non-blocking scan of prefix.1 .. prefix.254 in batches of 28 sockets
    const int BATCH_SIZE = 28;
    int portsToTry[2] = { targetPort, (targetPort == 8193 ? 8191 : 8193) };

    for (int pIdx = 0; pIdx < 2 && !stopDiscovery_; pIdx++) {
        int port = portsToTry[pIdx];
        for (int startHost = 1; startHost <= 254 && !stopDiscovery_; startHost += BATCH_SIZE) {
            int endHost = std::min(254, startHost + BATCH_SIZE - 1);
            {
                std::lock_guard<std::mutex> lock(discoveryMutex_);
                discoveryStatus_ = "Taraniyor: " + prefix + std::to_string(startHost) + " - " + std::to_string(endHost) + " (:" + std::to_string(port) + ")";
            }

            struct BatchSock {
                int fd = -1;
                std::string ip;
            };
            std::vector<BatchSock> batch;
            batch.reserve(BATCH_SIZE);

            fd_set wfds;
            FD_ZERO(&wfds);
            int maxFd = -1;

            for (int h = startHost; h <= endHost; h++) {
                std::string ip = prefix + std::to_string(h);
                int s = socket(AF_INET, SOCK_STREAM, 0);
                if (s < 0) continue;

                int flags = fcntl(s, F_GETFL, 0);
                if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);

                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

                ::connect(s, (struct sockaddr*)&addr, sizeof(addr));
                FD_SET(s, &wfds);
                if (s > maxFd) maxFd = s;
                batch.push_back({s, ip});
            }

            if (maxFd >= 0) {
                struct timeval tv = {0, 120000}; // 120ms per batch of 28 IPs
                int ready = select(maxFd + 1, nullptr, &wfds, nullptr, &tv);
                std::vector<std::string> candidateIps;

                if (ready > 0) {
                    for (const auto& item : batch) {
                        if (FD_ISSET(item.fd, &wfds)) {
                            int soErr = 0;
                            socklen_t errLen = sizeof(soErr);
                            if (getsockopt(item.fd, SOL_SOCKET, SO_ERROR, &soErr, &errLen) == 0 && soErr == 0) {
                                candidateIps.push_back(item.ip);
                            }
                        }
                    }
                }

                for (const auto& item : batch) {
                    close(item.fd);
                }

                for (const auto& candIp : candidateIps) {
                    if (stopDiscovery_) break;
                    if (probeMacroDeckHttp(candIp, port, 300)) {
                        std::lock_guard<std::mutex> lock(discoveryMutex_);
                        discoveredHost_ = candIp;
                        discoveredPort_ = port;
                        discoveryStatus_ = "Bulundu: " + candIp + ":" + std::to_string(port) + " (Baglan'i secin)";
                        hasDiscoveredServer_ = true;
                        discovering_ = false;
                        return;
                    }
                }
            } else {
                for (const auto& item : batch) {
                    if (item.fd >= 0) close(item.fd);
                }
            }
        }
    }

    if (!stopDiscovery_) {
        std::lock_guard<std::mutex> lock(discoveryMutex_);
        discoveryStatus_ = "Agda acik Macro Deck sunucusu bulunamadi (" + prefix + "x)";
    }
    discovering_ = false;
}

// ============================================================================
// Higher-Level WebSocket & HTTP Methods
// ============================================================================

void WebSocketClient::sendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (state_ == WsState::Connected) outgoingQueue_.push(text);
}

std::vector<std::string> WebSocketClient::syncFolderAndWidgets(const std::string& folderId, const std::vector<std::string>& widgetIds) {
    AppConfig cfg = getCurrentConfig();
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (state_ != WsState::Connected) return {};
    if (!folderId.empty() && folderId != reportedFolder_) {
        nlohmann::json rep;
        rep["protocolVersion"] = 1;
        rep["kind"] = "request";
        rep["type"] = "ReportFolderChanged";
        rep["id"] = "folder-" + std::to_string(++reqSeq_);
        rep["payload"] = nlohmann::json::array({{
            {"folderId", folderId},
            {"clientId", cfg.client_id},
            {"isResync", true}
        }});
        outgoingQueue_.push(rep.dump());
        reportedFolder_ = folderId;
    }

    displayedWidgets_ = {widgetIds.begin(), widgetIds.end()};
    std::vector<std::string> closedSessions;
    for (auto it = sessionToWidget_.begin(); it != sessionToWidget_.end();) {
        if (displayedWidgets_.count(it->second)) {
            ++it;
            continue;
        }
        queueSessionClose(it->first);
        closedSessions.push_back(it->first);
        widgetSessionAttempts_.erase(it->second);
        it = sessionToWidget_.erase(it);
    }
    syncWidgetSessions();
    return closedSessions;
}

void WebSocketClient::syncWidgetSessions() {
    const auto now = std::chrono::steady_clock::now();
    std::set<std::string> sessions;
    for (auto it = sessionToWidget_.begin(); it != sessionToWidget_.end();) {
        const auto attempt = widgetSessionAttempts_.find(it->second);
        if (attempt != widgetSessionAttempts_.end() && now >= attempt->second.deadline) {
            const auto sessionId = it->first;
            const auto widgetId = it->second;
            ++it;
            retryWidgetSession(widgetId, sessionId);
        } else {
            sessions.insert(it->second);
            ++it;
        }
    }
    for (auto it = widgetSessionAttempts_.begin(); it != widgetSessionAttempts_.end();) {
        if (!displayedWidgets_.count(it->first) || now >= it->second.deadline) {
            it = widgetSessionAttempts_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& widgetId : displayedWidgets_) {
        if (widgetId.empty() || sessions.count(widgetId) || widgetSessionAttempts_.count(widgetId)) continue;
        const std::string id = "open-" + std::to_string(++reqSeq_);
        widgetSessionAttempts_[widgetId] = {id, now + std::chrono::seconds(5)};
        nlohmann::json request = {
            {"protocolVersion", 1}, {"kind", "request"}, {"type", "OpenWidgetUiSession"}, {"id", id},
            {"payload", nlohmann::json::array({{{"widgetId", widgetId}}})}
        };
        outgoingQueue_.push(request.dump());
    }
}

void WebSocketClient::queueSessionAttach(const std::string& widgetId, const std::string& sessionId) {
    const std::string id = "attach-" + std::to_string(++reqSeq_);
    widgetSessionAttempts_[widgetId] = {id, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
    nlohmann::json request = {
        {"protocolVersion", 1}, {"kind", "request"}, {"type", "AttachUiSession"}, {"id", id},
        {"payload", nlohmann::json::array({{{"sessionId", sessionId}}})}
    };
    outgoingQueue_.push(request.dump());
}

void WebSocketClient::queueSessionClose(const std::string& sessionId) {
    nlohmann::json request = {
        {"protocolVersion", 1}, {"kind", "request"}, {"type", "CloseUiSession"},
        {"id", "close-" + std::to_string(++reqSeq_)}, {"payload", nlohmann::json::array({sessionId})}
    };
    outgoingQueue_.push(request.dump());
}

void WebSocketClient::retryWidgetSession(const std::string& widgetId, const std::string& sessionId) {
    if (!sessionId.empty()) {
        queueSessionClose(sessionId);
        sessionToWidget_.erase(sessionId);
        const nlohmann::json ended = {{"_internal", "MD3_WIDGET_SESSION_ENDED"}, {"sessionId", sessionId}};
        incomingQueue_.push(ended.dump());
    }
    widgetSessionAttempts_[widgetId] = {{}, std::chrono::steady_clock::now() + std::chrono::seconds(2)};
}

void WebSocketClient::resyncWidgetSession(const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    const auto session = sessionToWidget_.find(sessionId);
    if (state_ != WsState::Connected || session == sessionToWidget_.end() ||
        widgetSessionAttempts_.count(session->second)) return;
    queueSessionAttach(session->second, sessionId);
}

void WebSocketClient::invalidateWidget(const std::string& widgetId) {
    if (widgetId.empty()) return;
    std::lock_guard<std::mutex> lock(queueMutex_);
    for (auto it = sessionToWidget_.begin(); it != sessionToWidget_.end();) {
        if (it->second == widgetId) {
            queueSessionClose(it->first);
            it = sessionToWidget_.erase(it);
        } else {
            ++it;
        }
    }
    widgetSessionAttempts_.erase(widgetId);
    if (state_ == WsState::Connected) syncWidgetSessions();
}

bool WebSocketClient::hasWidgetSession(const std::string& widgetId) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return std::any_of(sessionToWidget_.begin(), sessionToWidget_.end(), [&](const auto& session) {
        return session.second == widgetId;
    });
}

bool WebSocketClient::handleUiSessionMessage(const nlohmann::json& envelope) {
    const auto kind = jsonStr(envelope, "kind");
    const auto type = jsonStr(envelope, "type");
    if (kind == "message") {
        const auto body = envelope.find("payload");
        if (body == envelope.end() || !body->is_object()) return false;
        if (type == "UiSessionTreeUpdatedEvent" && body->contains("tree")) {
            const auto session = sessionToWidget_.find(jsonStr(*body, "sessionId"));
            if (session != sessionToWidget_.end()) widgetSessionAttempts_.erase(session->second);
        } else if (type == "UiSessionClosedEvent" || type == "UiSessionInvalidatedEvent") {
            const auto session = sessionToWidget_.find(jsonStr(*body, "sessionId"));
            if (session != sessionToWidget_.end()) {
                widgetSessionAttempts_[session->second] = {{}, std::chrono::steady_clock::now() + std::chrono::seconds(2)};
                sessionToWidget_.erase(session);
            }
        }
        return false;
    }
    if (kind != "response" && kind != "error") return false;

    const auto id = jsonStr(envelope, "correlationId", jsonStr(envelope, "id"));
    const bool opening = id.rfind("open-", 0) == 0;
    const bool uiEvent = id.rfind("ev-", 0) == 0;
    if (!uiEvent && !opening && id.rfind("attach-", 0) != 0) return false;
    const auto payload = envelope.find("payload");
    const nlohmann::json* body = payload == envelope.end() ? nullptr : &*payload;
    if (body && body->is_array()) body = body->empty() ? nullptr : &(*body)[0];
    const bool accepted = kind == "response" && body && jsonBool(*body, "accepted", false);
    if (uiEvent) {
        if (accepted) return true;
        const auto separator = id.find(':');
        if (separator == std::string::npos) return true;
        const auto session = sessionToWidget_.find(id.substr(separator + 1));
        if (session == sessionToWidget_.end()) return true;
        const bool synchronizing = widgetSessionAttempts_.count(session->second) != 0;
        if (synchronizing && id.rfind("ev-quiet-", 0) == 0) return true;

        const auto error = envelope.find("error");
        const auto code = body ? jsonStr(*body, "code")
                               : error == envelope.end() ? std::string{} : jsonStr(*error, "code");
        reportActionError(code == "HOST_LOCKED" ? "host-locked" : "widget-rejected");
        // Refresh before the next press. Repeating the original command could trigger it twice.
        if (code != "HOST_LOCKED" && !synchronizing) {
            queueSessionAttach(session->second, session->first);
        }
        return true;
    }

    const auto attempt = std::find_if(widgetSessionAttempts_.begin(), widgetSessionAttempts_.end(),
        [&](const auto& entry) { return entry.second.requestId == id; });
    const std::string sessionId = body ? jsonStr(*body, "sessionId") : std::string{};

    if (attempt == widgetSessionAttempts_.end()) {
        // A page change or timeout can retire an open request before the reply arrives.
        if (opening && !sessionId.empty() && !sessionToWidget_.count(sessionId)) queueSessionClose(sessionId);
        return true;
    }
    const auto widgetId = attempt->first;
    if (opening && accepted && !sessionId.empty()) {
        sessionToWidget_[sessionId] = widgetId;
        const nlohmann::json opened = {
            {"_internal", "MD3_WIDGET_SESSION"}, {"widgetId", widgetId}, {"sessionId", sessionId}
        };
        incomingQueue_.push(opened.dump());
        queueSessionAttach(widgetId, sessionId);
    } else if (opening) {
        retryWidgetSession(widgetId, {});
    } else if (!accepted) {
        const auto session = std::find_if(sessionToWidget_.begin(), sessionToWidget_.end(),
            [&](const auto& entry) { return entry.second == widgetId; });
        retryWidgetSession(widgetId, session == sessionToWidget_.end() ? std::string{} : session->first);
    }
    return true;
}

void WebSocketClient::sendUiEvent(const std::string& sessionId, const std::string& nodeId, const std::string& eventName, const nlohmann::json& data, int revision) {
    if (sessionId.empty() || nodeId.empty()) return;
    if (!isConnected()) {
        reportActionError("disconnected");
        return;
    }
    nlohmann::json evObj;
    evObj["sessionId"] = sessionId;
    evObj["nodeId"] = nodeId;
    evObj["name"] = eventName;
    if (!data.is_null()) {
        evObj["data"] = data;
    }
    evObj["revision"] = revision;

    nlohmann::json req;
    req["protocolVersion"] = 1;
    req["kind"] = "request";
    req["type"] = "SendUiEvent";
    const bool quiet = eventName == "adjust" || eventName == "press-end";
    req["id"] = (quiet ? "ev-quiet-" : "ev-") + std::to_string(++reqSeq_) + ":" + sessionId;
    req["payload"] = nlohmann::json::array({ evObj });
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (state_ == WsState::Connected && sessionToWidget_.count(sessionId)) {
        outgoingQueue_.push(req.dump());
    } else if (!quiet) {
        reportActionError(state_ == WsState::Connected ? "widget-reconnecting" : "disconnected");
    }
}

void WebSocketClient::executeAction(const std::string& widgetId, const std::string& folderId, const std::string& triggerType) {
    if (widgetId.empty()) return;
    {
        std::lock_guard<std::mutex> lock(httpMutex_);
        if (!running_ || state_ != WsState::Connected) {
            reportActionError("disconnected");
            return;
        }
        actionQueue_.push({widgetId, folderId, triggerType, connection_});
    }
    httpCv_.notify_all();
}

void WebSocketClient::requestResourceImage(const std::string& widgetId, const std::string& resourceId, bool musicCover) {
    if (widgetId.empty() || resourceId.empty() || !isConnected()) return;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (requestedResources_.count(resourceId)) return;
        if (const auto retry = resourceRetryAfter_.find(resourceId); retry != resourceRetryAfter_.end()) {
            if (std::chrono::steady_clock::now() < retry->second) return;
            resourceRetryAfter_.erase(retry);
        }
        requestedResources_.insert(resourceId);
    }
    {
        std::lock_guard<std::mutex> lock(httpMutex_);
        httpTaskQueue_.push({HttpTask::Type::FetchResource, widgetId, resourceId, musicCover});
    }
    httpCv_.notify_all();
}

void WebSocketClient::requestDeckReload() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        reportedFolder_.clear();
        resourceRetryAfter_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(httpMutex_);
        HttpTask task;
        task.type = HttpTask::Type::ReloadDeck;
        httpTaskQueue_.push(task);
    }
    httpCv_.notify_all();
}

void WebSocketClient::requestAppearanceFonts() {
    if (!isConnected()) return;
    {
        std::lock_guard<std::mutex> lock(httpMutex_);
        HttpTask task{};
        task.type = HttpTask::Type::AppearanceFonts;
        httpTaskQueue_.push(task);
    }
    httpCv_.notify_all();
}

void WebSocketClient::updateFolderLayout(const std::string& folderId, int rows, int cols, int spacing, int radius) {
    if (folderId.empty()) return;
    {
        std::lock_guard<std::mutex> lock(httpMutex_);
        HttpTask task;
        task.type = HttpTask::Type::UpdateFolderLayout;
        task.arg1 = folderId;
        task.intVal1 = rows;
        task.intVal2 = cols;
        task.intVal3 = spacing;
        task.intVal4 = radius;
        httpTaskQueue_.push(task);
    }
    httpCv_.notify_all();
}

void WebSocketClient::poll() {
    std::queue<std::string> messagesToDispatch;
    std::queue<ResourceImageResult> resourcesToDispatch;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        messagesToDispatch.swap(incomingQueue_);
        resourcesToDispatch.swap(resourceResultQueue_);
    }

    while (!messagesToDispatch.empty()) {
        const std::string& m = messagesToDispatch.front();
        if (m.rfind("{\"_internal\":", 0) == 0) {
            try {
                auto j = nlohmann::json::parse(m);
                const auto type = jsonStr(j, "_internal");
                if (type == "WS_STATE") {
                    if (onState_) onState_(static_cast<WsState>(jsonInt(j, "state", 0)), jsonStr(j, "msg"));
                    messagesToDispatch.pop();
                    continue;
                }
                if (type == "MD3_WIDGET_SESSION") {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    if (!sessionToWidget_.count(jsonStr(j, "sessionId"))) {
                        messagesToDispatch.pop();
                        continue;
                    }
                }
            } catch (...) {}
        }

        if (onMessage_) {
            onMessage_(m);
        }
        messagesToDispatch.pop();
    }

    while (!resourcesToDispatch.empty()) {
        const auto& resItem = resourcesToDispatch.front();
        if (onResource_) {
            onResource_(resItem);
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            requestedResources_.erase(resItem.resourceId);
        }
        resourcesToDispatch.pop();
    }
}

bool WebSocketClient::httpRequest(const std::string& method, const std::string& host, int port, const std::string& path,
                                  bool useAuth, const std::string& extraHeaders, const std::string& body,
                                  int& outStatusCode, std::string& outBody) {
    outStatusCode = 0;
    outBody.clear();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval tv = {1, 500000}; // 1.5s timeout for fast failover
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host.c_str());
        if (he != nullptr) {
            memcpy(&serv_addr.sin_addr, he->h_addr_list[0], he->h_length);
        } else {
            close(sock);
            return false;
        }
    }

    if (!connectWithTimeout(sock, serv_addr, 1500)) {
        close(sock);
        return false;
    }

    AppConfig cfg = getCurrentConfig();

    // Use HTTP/1.0 so Kestrel does not use chunked encoding and closes immediately after sending body
    std::ostringstream req;
    req << method << " " << path << " HTTP/1.0\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "User-Agent: SwitchDeck/2.0\r\n";
    if (method == "POST" || !body.empty()) {
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << body.length() << "\r\n";
    }
    if (useAuth && !cfg.access_token.empty()) {
        req << "Authorization: Bearer " << cfg.access_token << "\r\n";
    }
    if (!cfg.refresh_cookie.empty()) {
        req << "Cookie: " << cfg.refresh_cookie << "\r\n";
    }
    if (!extraHeaders.empty()) {
        req << extraHeaders << "\r\n";
    }
    req << "Connection: close\r\n\r\n";
    if (!body.empty()) {
        req << body;
    }

    std::string reqStr = req.str();
    size_t totalSent = 0;
    while (totalSent < reqStr.size()) {
        ssize_t s = send(sock, reqStr.data() + totalSent, reqStr.size() - totalSent, 0);
        if (s <= 0) {
            close(sock);
            return false;
        }
        totalSent += static_cast<size_t>(s);
    }

    std::string respStr;
    char buffer[4096];
    while (running_) {
        int bytes = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;
        constexpr size_t maxResponseBytes = 8 * 1024 * 1024;
        if (static_cast<size_t>(bytes) > maxResponseBytes - respStr.size()) {
            close(sock);
            return false;
        }
        respStr.append(buffer, bytes);
    }
    close(sock);
    if (!running_) return false;

    size_t headerEnd = respStr.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;

    std::string headers = respStr.substr(0, headerEnd);
    outBody = respStr.substr(headerEnd + 4);

    // Parse HTTP Status Code
    size_t space1 = headers.find(' ');
    if (space1 != std::string::npos && space1 + 4 <= headers.size()) {
        try {
            outStatusCode = std::stoi(headers.substr(space1 + 1, 3));
        } catch (...) {}
    }

    // Parse Set-Cookie if present
    std::istringstream hStream(headers);
    std::string hLine;
    bool isChunked = false;
    while (std::getline(hStream, hLine)) {
        if (!hLine.empty() && hLine.back() == '\r') hLine.pop_back();
        std::string lower = hLine;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lower.find("set-cookie:") == 0 && outStatusCode == 200 &&
            (path == "/api/auth/refresh" || path == "/api/auth/device-enrollment/redeem")) {
            std::string val = hLine.substr(11);
            while (!val.empty() && val.front() == ' ') val.erase(0, 1);
            size_t semi = val.find(';');
            std::string cookiePair = (semi != std::string::npos) ? val.substr(0, semi) : val;
            if (!cookiePair.empty() && cookiePair.find('=') != std::string::npos) {
                std::lock_guard<std::mutex> lock(configMutex_);
                config_.refresh_cookie = cookiePair;
                ConfigManager::save(config_);
            }
        } else if (lower.find("transfer-encoding:") == 0 && lower.find("chunked") != std::string::npos) {
            isChunked = true;
        }
    }

    if (isChunked) {
        outBody = decodeChunkedBody(outBody);
    }

    // Automatically refresh JWT access token if an authenticated request returns 401
    if (running_ && useAuth && outStatusCode == 401) {
        const auto result = tryRefreshToken(cfg.access_token);
        if (result == RefreshResult::Ready) {
            return httpRequest(method, host, port, path, false /* prevent infinite recursion */,
                               "Authorization: Bearer " + getCurrentConfig().access_token +
                               (extraHeaders.empty() ? "" : ("\r\n" + extraHeaders)),
                               body, outStatusCode, outBody);
        }
    }

    return true;
}

WebSocketClient::RefreshResult WebSocketClient::tryRefreshToken(const std::string& rejectedToken) {
    std::lock_guard<std::mutex> refreshLock(refreshMutex_);
    const AppConfig cfg = getCurrentConfig();
    if (!cfg.access_token.empty() && cfg.access_token != rejectedToken) return RefreshResult::Ready;
    if (!running_) return RefreshResult::Unavailable;
    if (cfg.refresh_cookie.empty()) return RefreshResult::Rejected;

    int code = 0;
    std::string body;
    if (!httpRequest("POST", cfg.host, cfg.port, "/api/auth/refresh", false, "", "", code, body)) {
        return RefreshResult::Unavailable;
    }
    if (code == 401 || code == 403) return RefreshResult::Rejected;
    if (code != 200) return RefreshResult::Unavailable;

    try {
        auto j = nlohmann::json::parse(cleanJsonString(body));
        std::string newToken = jsonStr(j, "accessToken");
        if (!newToken.empty()) {
            std::lock_guard<std::mutex> lock(configMutex_);
            config_.access_token = newToken;
            if (j.is_object() && j.contains("device") && j["device"].is_object()) {
                std::string did = jsonStr(j["device"], "deviceId");
                std::string dsec = jsonStr(j["device"], "deviceSecret");
                std::string spid = jsonStr(j["device"], "startupProfileId");
                if (!did.empty()) config_.device_id = did;
                if (!dsec.empty()) config_.device_secret = dsec;
                if (config_.startup_profile_id.empty()) config_.startup_profile_id = spid;
            }
            ConfigManager::save(config_);
            return RefreshResult::Ready;
        }
    } catch (...) {}
    return RefreshResult::Unavailable;
}

void WebSocketClient::fetchAppearanceFonts() {
    const auto config = getCurrentConfig();
    int code = 0;
    std::string body;
    if (!httpRequest("GET", config.host, config.port, "/api/settings/appearance", true, "", "", code, body) || code != 200) return;
    try {
        const auto family = jsonStr(nlohmann::json::parse(body), "fontFamily");
        std::string regular, bold;
        if (!family.empty()) {
            if (!httpRequest("GET", config.host, config.port, "/api/system/fonts", true, "", "", code, body) || code != 200) return;
            const auto catalog = nlohmann::json::parse(body);
            const auto faces = catalog.find("faces");
            if (faces != catalog.end() && faces->is_array()) {
                int regularDistance = 1000, boldDistance = 1000;
                for (const auto& face : *faces) {
                    if (jsonStr(face, "family") != family || jsonStr(face, "slant") != "upright" ||
                        !jsonBool(face, "remoteRenderable")) continue;
                    const auto id = jsonStr(face, "faceId");
                    if (id.empty() || id.size() > 160) continue;
                    const int weight = std::clamp(jsonInt(face, "weight", 400), 100, 900);
                    if (const int distance = std::abs(weight - 400); distance < regularDistance) {
                        regular = id;
                        regularDistance = distance;
                    }
                    if (const int distance = std::abs(weight - 700); weight >= 600 && distance < boldDistance) {
                        bold = id;
                        boldDistance = distance;
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(queueMutex_);
        incomingQueue_.push(nlohmann::json{{"_internal", "MD3_APPEARANCE_FONTS"}, {"regular", regular}, {"bold", bold}}.dump());
    } catch (...) {}
}

bool WebSocketClient::fetchDeckFromServer() {
    AppConfig cfg = getCurrentConfig();
    int code = 0;
    std::string localizationBody;
    if (httpRequest("GET", cfg.host, cfg.port, "/api/localization", true, "", "", code, localizationBody) && code == 200) {
        try {
            auto catalog = nlohmann::json::parse(cleanJsonString(localizationBody));
            std::lock_guard<std::mutex> lock(queueMutex_);
            incomingQueue_.push(nlohmann::json{{"_internal", "MD3_LOCALIZATION"}, {"catalog", std::move(catalog)}}.dump());
        } catch (...) {}
    }
    std::string profilesBody;
    if (!httpRequest("GET", cfg.host, cfg.port, "/api/profiles", true, "", "", code, profilesBody) || code != 200) {
        return false;
    }

    std::string targetProfileId;
    nlohmann::json profiles;
    try {
        profiles = nlohmann::json::parse(cleanJsonString(profilesBody));
        if (profiles.is_object()) {
            if (profiles.contains("profiles")) {
                profiles = nlohmann::json(profiles["profiles"]);
            } else if (profiles.contains("items")) {
                profiles = nlohmann::json(profiles["items"]);
            }
        }
        if (!profiles.is_array()) return false;

        std::string firstId;
        std::string defaultId;
        for (const auto& profile : profiles) {
            const auto id = jsonStr(profile, "id");
            if (id.empty()) continue;
            if (firstId.empty()) firstId = id;
            if (jsonBool(profile, "isDefault")) defaultId = id;
            if (id == cfg.startup_profile_id) {
                targetProfileId = id;
                break;
            }
        }
        if (targetProfileId.empty()) targetProfileId = defaultId.empty() ? firstId : defaultId;
    } catch (...) {
        return false;
    }

    std::string foldersPath = "/api/folders";
    if (!targetProfileId.empty()) {
        foldersPath += "?profileId=" + urlEncode(targetProfileId);
    }

    std::string foldersBody;
    if (!httpRequest("GET", cfg.host, cfg.port, foldersPath, true, "", "", code, foldersBody) || code != 200) {
        return false;
    }

    try {
        auto folders = nlohmann::json::parse(cleanJsonString(foldersBody));
        if (!folders.is_array() &&
            !(folders.is_object() && ((folders.contains("folders") && folders["folders"].is_array()) ||
                                      (folders.contains("items") && folders["items"].is_array())))) {
            return false;
        }
        nlohmann::json snapshot = {
            {"_internal", "MD3_DECK"},
            {"requestedProfileId", cfg.startup_profile_id},
            {"profileId", targetProfileId},
            {"profiles", std::move(profiles)},
            {"folders", std::move(folders)}
        };
        std::lock_guard<std::mutex> lock(queueMutex_);
        incomingQueue_.push(snapshot.dump());
        return true;
    } catch (...) {
        return false;
    }
}

static bool randomBytes(uint8_t* bytes, size_t length) {
#ifdef __SWITCH__
    return R_SUCCEEDED(csrngGetRandomBytes(bytes, length));
#else
    std::random_device random;
    for (size_t i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(random());
    return true;
#endif
}

static std::string encodeHandshakeBytes(const uint8_t* bytes, size_t length) {
    unsigned char encoded[32];
    size_t written = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &written, bytes, length) != 0) return {};
    return std::string(reinterpret_cast<char*>(encoded), written);
}

static bool hasHttpToken(const std::string& field, const std::string& expected) {
    std::istringstream tokens(lowerAscii(field));
    std::string token;
    while (std::getline(tokens, token, ',')) {
        if (trimHttpWhitespace(token) == expected) return true;
    }
    return false;
}

static bool validWebSocketUpgrade(const std::string& response, const std::string& key, const std::string& subprotocol) {
    std::istringstream lines(response);
    std::string line;
    if (!std::getline(lines, line)) return false;
    std::istringstream statusLine(line);
    std::string version;
    std::string status;
    if (!(statusLine >> version >> status) || version != "HTTP/1.1" || status != "101") return false;

    bool upgrade = false;
    bool connection = false;
    std::string accept;
    std::string protocol;
    bool hasProtocol = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string::npos) return false;
        const auto name = lowerAscii(line.substr(0, colon));
        const auto value = trimHttpWhitespace(line.substr(colon + 1));
        if (name == "upgrade") {
            upgrade = upgrade || hasHttpToken(value, "websocket");
        } else if (name == "connection") {
            connection = connection || hasHttpToken(value, "upgrade");
        } else if (name == "sec-websocket-accept") {
            if (!accept.empty() || value.empty()) return false;
            accept = value;
        } else if (name == "sec-websocket-protocol") {
            if (hasProtocol) return false;
            hasProtocol = true;
            protocol = value;
        } else if (name == "sec-websocket-extensions") {
            return false;
        }
    }
    if (!upgrade || !connection || accept.empty() ||
        (subprotocol.empty() ? hasProtocol : protocol != subprotocol)) return false;

    const std::string challenge = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[20];
    if (mbedtls_sha1_ret(reinterpret_cast<const unsigned char*>(challenge.data()), challenge.size(), digest) != 0) return false;
    return accept == encodeHandshakeBytes(digest, sizeof(digest));
}

bool WebSocketClient::doHandshake(int sock, const std::string& host, int port, const std::string& path, const std::string& subprotocol) {
    uint8_t nonce[16];
    if (!randomBytes(nonce, sizeof(nonce))) return false;
    const auto key = encodeHandshakeBytes(nonce, sizeof(nonce));
    if (key.empty()) return false;

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << ":" << port << "\r\n";
    req << "Upgrade: websocket\r\n";
    req << "Connection: Upgrade\r\n";
    req << "Sec-WebSocket-Key: " << key << "\r\n";
    req << "Sec-WebSocket-Version: 13\r\n";
    if (!subprotocol.empty()) req << "Sec-WebSocket-Protocol: " << subprotocol << "\r\n";
    req << "User-Agent: SwitchDeck/2.0\r\n\r\n";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    const auto request = req.str();
    if (!sendAll(sock, request.data(), request.size())) return false;

    // Do not consume any of the first WebSocket frame while reading HTTP headers.
    std::string response;
    while (response.size() < 4096 && running_ && std::chrono::steady_clock::now() < deadline) {
        char ch;
        if (recv(sock, &ch, 1, 0) != 1) return false;
        response.push_back(ch);
        const auto length = response.size();
        if (length >= 4 && response.compare(length - 4, 4, "\r\n\r\n") == 0) {
            return validWebSocketUpgrade(response, key, subprotocol);
        }
    }
    return false;
}

bool WebSocketClient::sendFrame(int sock, uint8_t opcode, const uint8_t* payload, size_t length) {
    std::vector<uint8_t> frame;
    frame.reserve(length + 14);
    frame.push_back(0x80 | (opcode & 0x0F));

    if (length <= 125) {
        frame.push_back(0x80 | static_cast<uint8_t>(length));
    } else if (length <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back((length >> 8) & 0xFF);
        frame.push_back(length & 0xFF);
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((length >> (i * 8)) & 0xFF);
        }
    }

    uint8_t mask[4];
    if (!randomBytes(mask, sizeof(mask))) return false;
    frame.insert(frame.end(), std::begin(mask), std::end(mask));

    for (size_t i = 0; i < length; i++) {
        frame.push_back(payload[i] ^ mask[i % 4]);
    }

    return sendAll(sock, frame.data(), frame.size());
}

bool WebSocketClient::readFrame(int sock, std::string& outMessage, int headerTimeoutMs) {
    outMessage.clear();
    constexpr size_t maxMessageBytes = 4 * 1024 * 1024;
    timeval headerTimeout{headerTimeoutMs / 1000, (headerTimeoutMs % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &headerTimeout, sizeof(headerTimeout));
    uint8_t header[2];
    if (recv(sock, reinterpret_cast<char*>(header), 1, 0) != 1) return false;

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    const auto receiveExact = [&](void* destination, size_t remaining) {
        auto* bytes = static_cast<char*>(destination);
        while (remaining > 0 && running_) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
            if (us <= 0) return false;
            timeval timeout{static_cast<long>(us / 1000000), static_cast<long>(us % 1000000)};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            const auto received = recv(sock, bytes, remaining, 0);
            if (received <= 0) return false;
            bytes += received;
            remaining -= static_cast<size_t>(received);
        }
        return remaining == 0;
    };
    if (!receiveExact(header + 1, 1)) return false;

    uint8_t messageOpcode = 0;
    while (running_) {
        const bool final = (header[0] & 0x80) != 0;
        const uint8_t opcode = header[0] & 0x0F;
        const bool control = (opcode & 0x08) != 0;
        if ((header[0] & 0x70) || (header[1] & 0x80)) return false;
        uint64_t length = header[1] & 0x7F;
        if (control && (!final || length > 125)) return false;
        if (length >= 126) {
            const size_t extendedSize = length == 126 ? 2 : 8;
            uint8_t extended[8];
            if (!receiveExact(extended, extendedSize)) return false;
            length = 0;
            for (size_t i = 0; i < extendedSize; ++i) length = (length << 8) | extended[i];
            if (length < (extendedSize == 2 ? 126 : 65536)) return false;
        }

        if (control) {
            uint8_t payload[125];
            if (!receiveExact(payload, static_cast<size_t>(length))) return false;
            if (opcode == 0x8) {
                if (length != 1) sendFrame(sock, 0x8, payload, static_cast<size_t>(length));
                return false;
            }
            if (opcode == 0x9) {
                if (!sendFrame(sock, 0xA, payload, static_cast<size_t>(length))) return false;
            } else if (opcode == 0xA) {
                if (awaitingPong_ && length == sizeof(pingSequence_) &&
                    std::memcmp(payload, &pingSequence_, sizeof(pingSequence_)) == 0) {
                    awaitingPong_ = false;
                    const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastPingSentTime_).count();
                    const int sample = std::max(1, static_cast<int>(rtt));
                    if (pingSamples_[0] == 0) pingSamples_.fill(sample);
                    pingSamples_[nextPingSample_] = sample;
                    nextPingSample_ = (nextPingSample_ + 1) % pingSamples_.size();
                    auto ordered = pingSamples_;
                    std::sort(ordered.begin(), ordered.end());
                    pingMs_ = ordered[1];
                }
            } else {
                return false;
            }
            if (messageOpcode == 0) return true;
        } else {
            if (opcode == 0x1 || opcode == 0x2) {
                if (messageOpcode != 0) return false;
                messageOpcode = opcode;
            } else if (opcode != 0 || messageOpcode == 0) {
                return false;
            }
            if (length > maxMessageBytes - outMessage.size()) return false;
            const auto offset = outMessage.size();
            outMessage.resize(offset + static_cast<size_t>(length));
            if (!receiveExact(outMessage.data() + offset, static_cast<size_t>(length))) return false;
            if (final) {
                if (messageOpcode == 0x2) outMessage.clear();
                return true;
            }
        }
        if (!receiveExact(header, sizeof(header))) return false;
    }
    return false;
}

void WebSocketClient::httpWorker() {
    while (running_) {
        HttpTask task;
        {
            std::unique_lock<std::mutex> lock(httpMutex_);
            httpCv_.wait(lock, [this]() { return !httpTaskQueue_.empty() || !running_; });
            if (!running_) break;
            task = httpTaskQueue_.front();
            httpTaskQueue_.pop();
        }

        AppConfig curCfg = getCurrentConfig();
        if (task.type == HttpTask::Type::FetchResource) {
            int c = 0;
            std::string imgBytes;
            const std::string& cacheKey = task.arg2;
            const auto version = cacheKey.find('\n');
            const auto resId = cacheKey.substr(0, version);
            std::string rPath;
            if (resId.rfind("font:", 0) == 0) {
                rPath = "/api/system/fonts/" + urlEncode(resId.substr(5)) + "/file";
            } else if (resId.rfind("icon:", 0) == 0) {
                rPath = "/icons/" + urlEncode(resId.substr(5)) + ".svg";
            } else {
                rPath = "/api/ui/resources/" + urlEncode(resId) + "?size=256";
                if (version != std::string::npos) rPath += "&v=" + urlEncode(cacheKey.substr(version + 1));
                if (resId.size() > 2 && resId.substr(resId.size() - 2) == ".a") {
                    rPath += (curCfg.theme == 1) ? "&colorScheme=light&motion=static" : "&colorScheme=dark&motion=static";
                }
            }
            const char* headers = task.musicCover ? "Cache-Control: no-cache" : "";
            if (httpRequest("GET", curCfg.host, curCfg.port, rPath, true, headers, "", c, imgBytes) && c == 200 && !imgBytes.empty()) {
                std::lock_guard<std::mutex> lock(queueMutex_);
                resourceResultQueue_.push({task.arg1, cacheKey, std::move(imgBytes)});
            } else {
                std::lock_guard<std::mutex> lock(queueMutex_);
                requestedResources_.erase(cacheKey);
                if (resourceRetryAfter_.size() >= 512) resourceRetryAfter_.erase(resourceRetryAfter_.begin());
                resourceRetryAfter_[cacheKey] = std::chrono::steady_clock::now() +
                    std::chrono::seconds(task.musicCover ? 2 : (c == 404 ? 120 : 15));
            }
        } else if (task.type == HttpTask::Type::ReloadDeck) {
            if (fetchDeckFromServer()) fetchAppearanceFonts();
            else if (isConnected()) reportActionError("deck-sync");
        } else if (task.type == HttpTask::Type::AppearanceFonts) {
            fetchAppearanceFonts();
        } else if (task.type == HttpTask::Type::UpdateFolderLayout) {
            nlohmann::json fBody;
            if (task.intVal1 > 0) fBody["rows"] = task.intVal1;
            if (task.intVal2 > 0) fBody["columns"] = task.intVal2;
            if (task.intVal3 >= 0) fBody["widgetSpacing"] = task.intVal3;
            if (task.intVal4 >= 0) fBody["widgetBorderRadius"] = task.intVal4;
            int c = 0;
            std::string b;
            std::string path = "/api/folders/" + urlEncode(task.arg1);
            httpRequest("PUT", curCfg.host, curCfg.port, path, true, "", fBody.dump(), c, b);
        }
    }
}

void WebSocketClient::actionWorker() {
    while (true) {
        ActionRequest action;
        {
            std::unique_lock<std::mutex> lock(httpMutex_);
            httpCv_.wait(lock, [this]() { return !running_ || !actionQueue_.empty(); });
            if (!running_) return;
            action = std::move(actionQueue_.front());
            actionQueue_.pop();
            if (action.connection != connection_ || state_ != WsState::Connected) continue;
        }

        const AppConfig config = getCurrentConfig();
        const nlohmann::json body = {
            {"widgetId", action.widgetId},
            {"folderId", action.folderId},
            {"triggerType", action.triggerType},
            {"clientId", config.client_id}
        };
        int status = 0;
        std::string response;
        const bool received = httpRequest("POST", config.host, config.port, "/api/actions/execute",
                                          true, "", body.dump(), status, response);
        if (!received || status < 200 || status >= 300) {
            reportActionError(status > 0 ? "HTTP " + std::to_string(status) : "unconfirmed");
        }
    }
}

void WebSocketClient::workerThread() {
    while (running_) {
        AppConfig cfg = getCurrentConfig();
        setStatus(WsState::Connecting, "Baglaniyor: " + cfg.host + ":" + std::to_string(cfg.port));

        std::string wsPath = "/";
        std::string wsSubprotocol = "";
        bool isMd3 = (cfg.port == 8193 || !cfg.pairing_code.empty() || !cfg.access_token.empty() || !cfg.refresh_cookie.empty());

        if (isMd3) {
            // Step 1: If user entered a pairing code, redeem it ONCE
            if (!cfg.pairing_code.empty()) {
                setStatus(WsState::Connecting, "Esleme Kodu Dogrulaniyor...");

                std::string codeToRedeem = cfg.pairing_code;
                // Clear pairing_code immediately so we NEVER send a one-time code twice!
                {
                    std::lock_guard<std::mutex> lock(configMutex_);
                    config_.pairing_code.clear();
                    ConfigManager::save(config_);
                    cfg = config_;
                }

                nlohmann::json redeemBody;
                redeemBody["token"] = codeToRedeem;
                nlohmann::json devObj;
                devObj["clientType"] = "web-client";
                devObj["proposedName"] = cfg.device_name;
                devObj["platform"] = "Nintendo Switch";
                devObj["browser"] = "SwitchDeck NRO";
                devObj["formFactor"] = "tablet";
                if (!cfg.device_id.empty()) devObj["deviceId"] = cfg.device_id;
                if (!cfg.device_secret.empty()) devObj["deviceSecret"] = cfg.device_secret;
                redeemBody["device"] = devObj;

                int status = 0;
                std::string respBody;
                if (!httpRequest("POST", cfg.host, cfg.port, "/api/auth/device-enrollment/redeem",
                                 false, "", redeemBody.dump(), status, respBody)) {
                    setStatus(WsState::Disconnected, "Sunucuya ulasilamadi (" + cfg.host + ":" + std::to_string(cfg.port) + ")");
                    if (!waitToReconnect(std::chrono::seconds(3))) break;
                    continue;
                }

                if (status == 200) {
                    try {
                        auto j = nlohmann::json::parse(cleanJsonString(respBody));
                        std::lock_guard<std::mutex> lock(configMutex_);
                        config_.access_token = jsonStr(j, "accessToken");
                        if (j.is_object() && j.contains("device") && j["device"].is_object()) {
                            std::string did = jsonStr(j["device"], "deviceId");
                            std::string dsec = jsonStr(j["device"], "deviceSecret");
                            std::string spid = jsonStr(j["device"], "startupProfileId");
                            if (!did.empty()) config_.device_id = did;
                            if (!dsec.empty()) config_.device_secret = dsec;
                            config_.startup_profile_id = spid;
                        }
                        ConfigManager::save(config_);
                        cfg = config_;
                    } catch (const std::exception& e) {
                        setStatus(WsState::Disconnected, std::string("Esleme JSON Hatasi: ") + e.what());
                        break;
                    } catch (...) {
                        setStatus(WsState::Disconnected, "Esleme yaniti okunamadi (JSON Hatasi)");
                        break;
                    }
                } else if (status == 429) {
                    setStatus(WsState::Disconnected, "Cok fazla deneme (429)! 1 dk bekleyip yeni kod girin.");
                    break;
                } else {
                    setStatus(WsState::Disconnected, "Kod gecersiz veya suresi dolmus! Macro Deck'ten yeni kod alin.");
                    break;
                }
            }

            // Step 2: Ensure we have an access token (try refresh if empty)
            cfg = getCurrentConfig();
            if (cfg.access_token.empty() && !cfg.refresh_cookie.empty()) {
                setStatus(WsState::Connecting, "Oturum Yenileniyor...");
                const auto result = tryRefreshToken();
                if (result == RefreshResult::Rejected) {
                    std::lock_guard<std::mutex> lock(configMutex_);
                    config_.refresh_cookie.clear();
                    ConfigManager::save(config_);
                } else if (result == RefreshResult::Unavailable) {
                    setStatus(WsState::Disconnected, "Oturum yenilenemedi");
                    if (!waitToReconnect(std::chrono::seconds(3))) break;
                    continue;
                }
                cfg = getCurrentConfig();
            }

            if (cfg.access_token.empty()) {
                setStatus(WsState::Disconnected, "Eslestirme gerekli: Ayarlar'dan 6 haneli kodu girin");
                break;
            }

            // Step 3: Mint UI WebSocket Ticket
            setStatus(WsState::Connecting, "WebSocket Bileti Aliniyor...");
            int ticketStatus = 0;
            std::string ticketBody;
            bool ticketOk = httpRequest("POST", cfg.host, cfg.port, "/api/ui-websocket/tickets",
                                        true, "X-MacroDeck-Ui-Protocol: 1", "{}", ticketStatus, ticketBody);

            if (ticketOk && ticketStatus == 401) {
                {
                    std::lock_guard<std::mutex> lock(configMutex_);
                    config_.access_token.clear();
                    config_.refresh_cookie.clear();
                    ConfigManager::save(config_);
                }
                setStatus(WsState::Disconnected, "Oturum suresi doldu: Ayarlar'dan yeni esleme kodu girin");
                break;
            }

            if (!ticketOk || ticketStatus != 200) {
                setStatus(WsState::Disconnected, "Bilet alinamadi (HTTP " + std::to_string(ticketStatus) + ")");
                if (!waitToReconnect(std::chrono::seconds(3))) break;
                continue;
            }

            try {
                auto tj = nlohmann::json::parse(cleanJsonString(ticketBody));
                std::string ticket = jsonStr(tj, "value");
                if (ticket.empty()) {
                    setStatus(WsState::Disconnected, "Sunucu bos bilet dondurdu");
                    if (!waitToReconnect(std::chrono::seconds(3))) break;
                    continue;
                }
                wsPath = "/ws/ui?ticket=" + urlEncode(ticket);
                wsSubprotocol = "macrodeck.ui.v1";
            } catch (...) {
                setStatus(WsState::Disconnected, "Bilet JSON hatasi");
                if (!waitToReconnect(std::chrono::seconds(3))) break;
                continue;
            }
        }

        // Step 4: Open TCP Socket & Perform WebSocket Upgrade Handshake
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            if (!waitToReconnect(std::chrono::seconds(2))) break;
            continue;
        }

        struct timeval tv = {4, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(cfg.port);

        if (inet_pton(AF_INET, cfg.host.c_str(), &serv_addr.sin_addr) <= 0) {
            struct hostent* he = gethostbyname(cfg.host.c_str());
            if (he != nullptr) {
                memcpy(&serv_addr.sin_addr, he->h_addr_list[0], he->h_length);
            } else {
                close(sock);
                setStatus(WsState::Disconnected, "Sunucu IP Adresi Gecersiz: " + cfg.host);
                if (!waitToReconnect(std::chrono::seconds(3))) break;
                continue;
            }
        }

        if (!connectWithTimeout(sock, serv_addr, 1500)) {
            close(sock);
            setStatus(WsState::Disconnected, "Baglanti Reddedildi (" + cfg.host + ":" + std::to_string(cfg.port) + ")");
            if (!waitToReconnect(std::chrono::seconds(2))) break;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            if (!running_) {
                close(sock);
                break;
            }
            socket_ = sock;
        }
        const auto closeConnection = [&]() {
            std::lock_guard<std::mutex> lock(socketMutex_);
            close(sock);
            socket_ = -1;
        };

        if (!doHandshake(sock, cfg.host, cfg.port, wsPath, wsSubprotocol)) {
            closeConnection();
            setStatus(WsState::Disconnected, "WebSocket El Sikisma (Handshake) Basarisiz");
            if (!waitToReconnect(std::chrono::seconds(2))) break;
            continue;
        }

        // Step 5: Macro Deck 3 Protocol Handshake (hello -> welcome -> RegisterClient)
        if (isMd3) {
            nlohmann::json helloMsg = {
                {"protocolVersion", 1},
                {"kind", "hello"}
            };
            std::string helloStr = helloMsg.dump();
            if (!sendFrame(sock, 0x1, reinterpret_cast<const uint8_t*>(helloStr.data()), helloStr.size())) {
                closeConnection();
                setStatus(WsState::Disconnected, "Hello mesaji gonderilemedi");
                if (!waitToReconnect(std::chrono::seconds(2))) break;
                continue;
            }

            std::string welcomeFrame;
            bool gotWelcome = false;
            for (int attempt = 0; attempt < 5 && running_ && !gotWelcome; attempt++) {
                if (readFrame(sock, welcomeFrame, 3000) && !welcomeFrame.empty()) {
                    try {
                        auto wj = nlohmann::json::parse(welcomeFrame);
                        if (jsonStr(wj, "kind") == "welcome") {
                            gotWelcome = true;
                        }
                    } catch (...) {}
                }
            }

            if (!gotWelcome) {
                closeConnection();
                setStatus(WsState::Disconnected, "Macro Deck 3 Welcome yaniti alinamadi");
                if (!waitToReconnect(std::chrono::seconds(2))) break;
                continue;
            }

            // RegisterClient (2nd argument must be a string array of features: ["open-url"])
            nlohmann::json regMsg = {
                {"protocolVersion", 1},
                {"kind", "request"},
                {"type", "RegisterClient"},
                {"id", "1-reg"},
                {"payload", nlohmann::json::array({cfg.client_id, nlohmann::json::array({"open-url"})})}
            };
            std::string regStr = regMsg.dump();
            sendFrame(sock, 0x1, reinterpret_cast<const uint8_t*>(regStr.data()), regStr.size());
        }

        if (!running_) {
            closeConnection();
            break;
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            outgoingQueue_ = {};
            for (const auto& session : sessionToWidget_) {
                const nlohmann::json ended = {{"_internal", "MD3_WIDGET_SESSION_ENDED"}, {"sessionId", session.first}};
                incomingQueue_.push(ended.dump());
            }
            sessionToWidget_.clear();
            widgetSessionAttempts_.clear();
            displayedWidgets_.clear();
        }
        setStatus(WsState::Connected, "Baglandi: " + cfg.host + ":" + std::to_string(cfg.port));
        auto lastReceived = std::chrono::steady_clock::now();
        lastPingSentTime_ = lastReceived - std::chrono::seconds(4);
        awaitingPong_ = false;
        pingSamples_.fill(0);
        nextPingSample_ = 0;
        pingMs_ = -1;

        if (isMd3) {
            requestDeckReload();
        }

        auto nextSessionCheck = lastReceived;

        // Step 6: Active Communication Loop
        while (running_ && state_ == WsState::Connected) {
            // 6a. Send periodic WebSocket Ping every 3.5 seconds to measure live RTT
            auto now = std::chrono::steady_clock::now();
            if (now - lastReceived >= std::chrono::seconds(15)) break;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPingSentTime_).count() >= 3500) {
                lastPingSentTime_ = now;
                ++pingSequence_;
                awaitingPong_ = true;
                if (!sendFrame(sock, 0x9, reinterpret_cast<const uint8_t*>(&pingSequence_), sizeof(pingSequence_))) {
                    state_ = WsState::Disconnected;
                    break;
                }
            }

            // 6b. Send queued WebSocket frames
            std::queue<std::string> toSend;
            bool hasMoreOutgoing = false;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (isMd3 && now >= nextSessionCheck) {
                    syncWidgetSessions();
                    nextSessionCheck = now + std::chrono::seconds(1);
                }
                while (!outgoingQueue_.empty() && toSend.size() < 16) {
                    toSend.push(std::move(outgoingQueue_.front()));
                    outgoingQueue_.pop();
                }
                hasMoreOutgoing = !outgoingQueue_.empty();
            }

            while (!toSend.empty()) {
                const auto& msg = toSend.front();
                if (!sendFrame(sock, 0x1, reinterpret_cast<const uint8_t*>(msg.data()), msg.length())) {
                    state_ = WsState::Disconnected;
                    break;
                }
                toSend.pop();
            }

            if (state_ != WsState::Connected) break;

            // 6c. Poll socket readability with select() (20ms timeout)
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            struct timeval tvPoll = {0, hasMoreOutgoing ? 0 : 20000};
            int sel = select(sock + 1, &rfds, nullptr, nullptr, &tvPoll);
            if (sel < 0) {
                if (errno != EINTR && errno != EAGAIN) {
                    state_ = WsState::Disconnected;
                    break;
                }
                continue;
            }
            if (sel == 0) {
                continue;
            }

            std::string msg;
            if (readFrame(sock, msg, 3000)) {
                lastReceived = std::chrono::steady_clock::now();
                if (!msg.empty()) {
                    bool handledInternally = false;
                    if (isMd3) {
                        try {
                            auto env = nlohmann::json::parse(msg);
                            std::string kind = jsonStr(env, "kind");

                            if (kind == "ping") {
                                nlohmann::json pong = {
                                    {"protocolVersion", 1},
                                    {"kind", "pong"},
                                    {"correlationId", jsonStr(env, "id")}
                                };
                                std::string pongStr = pong.dump();
                                sendFrame(sock, 0x1, reinterpret_cast<const uint8_t*>(pongStr.data()), pongStr.size());
                                handledInternally = true;
                            } else {
                                std::lock_guard<std::mutex> lock(queueMutex_);
                                handledInternally = handleUiSessionMessage(env);
                            }
                        } catch (...) {}
                    }

                    if (!handledInternally) {
                        std::lock_guard<std::mutex> lock(queueMutex_);
                        incomingQueue_.push(msg);
                    }
                }
            } else {
                state_ = WsState::Disconnected;
                break;
            }
        }

        closeConnection();
        if (running_) {
            setStatus(WsState::Disconnected, "Baglanti koptu");
            if (!waitToReconnect(std::chrono::seconds(2))) break;
        }
    }
    stopWorkers();
}
