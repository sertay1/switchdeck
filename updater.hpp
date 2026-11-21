#pragma once
#include <string>
#ifndef SWITCHDECK_VERSION
#define SWITCHDECK_VERSION "1.0.0"
#endif
#define SWITCHDECK_VERSION_TAG "v" SWITCHDECK_VERSION

struct UpdateCheckResult {
    bool hasUpdate = false;
    bool error = false;
    std::string currentVersion = SWITCHDECK_VERSION_TAG;
    std::string latestVersion;
    std::string releaseNotes;
    std::string downloadUrl;
    std::string message;
};

class AppUpdater {
public:
    static void init();
    static void requestCheck(const std::string& currentVersion);
    static bool poll(UpdateCheckResult& outResult);
    static bool pollNewQr();
    static bool isChecking();
    static void cleanup();
};
