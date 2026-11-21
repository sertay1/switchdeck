#include "updater.hpp"
#include <curl/curl.h>
#include <json.hpp>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <array>
#include <charconv>
#include <optional>

#ifdef __SWITCH__
#include <switch.h>
#endif

static std::thread s_thread;
static std::mutex s_mutex;
static std::atomic<bool> s_stop{false};
static bool s_hasResult = false;
static bool s_isChecking = false;
static bool s_hasNewQr = false;
static UpdateCheckResult s_result;

static std::optional<std::array<int, 3>> parseReleaseVersion(const std::string& tag) {
    std::array<int, 3> version{};
    const char* part = tag.data() + (!tag.empty() && tag.front() == 'v');
    const char* end = tag.data() + tag.size();
    for (size_t i = 0; i < version.size(); ++i) {
        const auto result = std::from_chars(part, end, version[i]);
        if (result.ec != std::errc{} || version[i] < 0) return std::nullopt;
        if (i + 1 == version.size()) return result.ptr == end ? std::optional{version} : std::nullopt;
        if (result.ptr == end || *result.ptr != '.') return std::nullopt;
        part = result.ptr + 1;
    }
    return std::nullopt;
}

static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total);
    return total;
}

static int cancelTransfer(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return s_stop ? 1 : 0;
}

static bool configureHttps(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelTransfer);
    if (curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS)) != CURLE_OK) {
        return false;
    }
#ifdef __SWITCH__
    return curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/tls-ca-bundle.pem") == CURLE_OK;
#else
    return true;
#endif
}

void AppUpdater::init() {
    s_stop = false;
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void AppUpdater::requestCheck(const std::string& currentVersion) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_isChecking) return;
        s_isChecking = true;
        s_hasResult = false;
    }

    if (s_thread.joinable()) {
        s_thread.join();
    }

    s_thread = std::thread([currentVersion]() {
        UpdateCheckResult res;
        res.currentVersion = currentVersion;

#ifdef __SWITCH__
        // 1. Check if Switch is actually connected to the internet via nifm
        NifmInternetConnectionStatus nifmStatus = NifmInternetConnectionStatus_ConnectingUnknown1;
        Result rc = nifmGetInternetConnectionStatus(nullptr, nullptr, &nifmStatus);
        if (R_FAILED(rc) || nifmStatus != NifmInternetConnectionStatus_Connected) {
            res.error = false;
            res.hasUpdate = false;
            res.message = "İnternet bağlantısı yok (Çevrimdışı).";
            std::lock_guard<std::mutex> lk(s_mutex);
            s_result = res;
            s_hasResult = true;
            s_isChecking = false;
            return;
        }
#endif

        CURL* curl = curl_easy_init();
        if (!curl) {
            res.error = true;
            res.message = "Ağ modülü başlatılamadı.";
            std::lock_guard<std::mutex> lk(s_mutex);
            s_result = res;
            s_hasResult = true;
            s_isChecking = false;
            return;
        }

        std::string buffer;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "User-Agent: SwitchDeck-App");
        headers = curl_slist_append(headers, "Accept: application/vnd.github.v3+json");

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.github.com/repos/SertAy/SwitchDeck/releases/latest");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 4L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const bool tlsReady = configureHttps(curl);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        CURLcode cc = tlsReady ? curl_easy_perform(curl) : CURLE_SSL_CACERT_BADFILE;
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (cc != CURLE_OK || httpCode != 200) {
            if (httpCode == 404) {
                res.hasUpdate = false;
                res.error = true;
                res.message = "GitHub’da yayımlanmış bir sürüm bulunamadı.";
            } else {
                res.error = true;
                res.message = cc == CURLE_PEER_FAILED_VERIFICATION || cc == CURLE_SSL_CACERT_BADFILE
                    ? "GitHub TLS sertifikasi dogrulanamadi."
                    : "GitHub bağlantısı kurulamadı.";
            }
        } else {
            try {
                auto j = nlohmann::json::parse(buffer);
                std::string tag = j.value("tag_name", "");
                const auto latest = parseReleaseVersion(tag);
                const auto installed = parseReleaseVersion(currentVersion);
                if (!latest || !installed) {
                    res.error = true;
                    res.message = "GitHub sürüm bilgisi eksik.";
                }
                res.latestVersion = tag;
                res.releaseNotes = j.value("name", "");
                if (j.contains("assets") && j["assets"].is_array() && !j["assets"].empty()) {
                    res.downloadUrl = j["assets"][0].value("browser_download_url", "");
                }

                if (latest && installed && *latest > *installed) {
                    res.hasUpdate = true;
                    res.message = "Yeni sürüm bulundu: " + tag;
                } else if (!res.error) {
                    res.hasUpdate = false;
                    res.message = "Sürümünüz Güncel (" + currentVersion + ")";
                }
            } catch (...) {
                res.error = true;
                res.message = "GitHub sürüm bilgisi okunamadı.";
            }
        }

        // Fetch latest contact QR from GitHub repository and keep sdmc:/switch/SwitchDeck/contact_qr.png in sync
        const char* qrUrls[] = {
            "https://raw.githubusercontent.com/SertAy/SwitchDeck/main/contact_qr.png",
            "https://raw.githubusercontent.com/SertAy/SwitchDeck/main/SD_Karta_Atilacak/switch/SwitchDeck/contact_qr.png"
        };
        for (const char* qUrl : qrUrls) {
            if (s_stop) break;
            CURL* qCurl = curl_easy_init();
            if (!qCurl) continue;
            std::string qBuf;
            struct curl_slist* qHeaders = nullptr;
            qHeaders = curl_slist_append(qHeaders, "User-Agent: SwitchDeck-App");
            curl_easy_setopt(qCurl, CURLOPT_URL, qUrl);
            curl_easy_setopt(qCurl, CURLOPT_HTTPHEADER, qHeaders);
            curl_easy_setopt(qCurl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(qCurl, CURLOPT_WRITEDATA, &qBuf);
            curl_easy_setopt(qCurl, CURLOPT_TIMEOUT, 6L);
            curl_easy_setopt(qCurl, CURLOPT_CONNECTTIMEOUT, 4L);
            curl_easy_setopt(qCurl, CURLOPT_NOSIGNAL, 1L);
            const bool qrTlsReady = configureHttps(qCurl);
            curl_easy_setopt(qCurl, CURLOPT_FOLLOWLOCATION, 1L);

            CURLcode qCc = qrTlsReady ? curl_easy_perform(qCurl) : CURLE_SSL_CACERT_BADFILE;
            long qHttp = 0;
            curl_easy_getinfo(qCurl, CURLINFO_RESPONSE_CODE, &qHttp);
            curl_slist_free_all(qHeaders);
            curl_easy_cleanup(qCurl);

            if (qCc == CURLE_OK && qHttp == 200 && qBuf.size() > 100) {
                if (static_cast<unsigned char>(qBuf[0]) == 0x89 && qBuf.substr(1, 3) == "PNG") {
                    const char* targetPath = "sdmc:/switch/SwitchDeck/contact_qr.png";
                    bool isIdentical = false;
                    std::ifstream curFile(targetPath, std::ios::binary);
                    if (curFile.is_open()) {
                        std::string existing((std::istreambuf_iterator<char>(curFile)), std::istreambuf_iterator<char>());
                        if (existing == qBuf) {
                            isIdentical = true;
                        }
                    }
                    if (!isIdentical) {
                        std::ofstream outFile(targetPath, std::ios::binary | std::ios::trunc);
                        if (outFile.is_open()) {
                            outFile.write(qBuf.data(), qBuf.size());
                            outFile.close();
                            std::lock_guard<std::mutex> lk(s_mutex);
                            s_hasNewQr = true;
                        }
                    }
                    break;
                }
            }
        }

        std::lock_guard<std::mutex> lk(s_mutex);
        s_result = res;
        s_hasResult = true;
        s_isChecking = false;
    });
}

bool AppUpdater::poll(UpdateCheckResult& outResult) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_hasResult) {
        outResult = s_result;
        s_hasResult = false;
        return true;
    }
    return false;
}

bool AppUpdater::pollNewQr() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_hasNewQr) {
        s_hasNewQr = false;
        return true;
    }
    return false;
}

bool AppUpdater::isChecking() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_isChecking;
}

void AppUpdater::cleanup() {
    s_stop = true;
    if (s_thread.joinable()) {
        s_thread.join();
    }
    curl_global_cleanup();
}
