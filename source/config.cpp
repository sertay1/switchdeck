#include "config.hpp"
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <iostream>
#include <random>
#include <ctime>
#include <algorithm>

#ifdef __SWITCH__
#include <switch.h>
#include <unistd.h>
#endif

std::string ConfigManager::getConfigPath() {
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchDeck", 0777);
    return "sdmc:/switch/SwitchDeck/config.ini";
#else
    return "config.ini";
#endif
}

std::string ConfigManager::generateClientId() {
    static const char hex[] = "0123456789abcdef";
    std::mt19937 rng(static_cast<unsigned int>(std::time(nullptr)) ^ 0x5A5A5A5Au);
    std::string out = "switchdeck-";
    for (int i = 0; i < 12; i++) {
        out.push_back(hex[rng() % 16]);
    }
    return out;
}

static int parseIntSafe(const std::string& val, int def, int minVal, int maxVal) {
    try {
        int v = std::stoi(val);
        return std::max(minVal, std::min(maxVal, v));
    } catch (...) {
        return def;
    }
}

AppConfig ConfigManager::load() {
    AppConfig config;
    std::string path = getConfigPath();
    std::ifstream file(path);
    if (!file.is_open()) {
        file.clear();
        file.open(path + ".bak");
    }
    if (!file.is_open()) {
        config.client_id = generateClientId();
        save(config);
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r')) key.pop_back();
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(0, 1);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);

        if (key == "host") config.host = val;
        else if (key == "port") config.port = parseIntSafe(val, 8193, 1, 65535);
        else if (key == "device_name") config.device_name = val;
        else if (key == "pairing_code") config.pairing_code = val;
        else if (key == "device_id") config.device_id = val;
        else if (key == "device_secret") config.device_secret = val;
        else if (key == "access_token") config.access_token = val;
        else if (key == "refresh_cookie") config.refresh_cookie = val;
        else if (key == "startup_profile_id") config.startup_profile_id = val;
        else if (key == "client_id") config.client_id = val;
        else if (key == "auto_reconnect") config.auto_reconnect = (val == "true" || val == "1");

        // Appearance & Grid (controlled strictly by Macro Deck 3 PC app for rows/cols/scale/spacing/radius)
        else if (key == "language") config.language = parseIntSafe(val, 0, 0, 1);
        else if (key == "theme") config.theme = parseIntSafe(val, 0, 0, 2);
        else if (key == "show_empty_cells") config.show_empty_cells = parseIntSafe(val, -1, -1, 1);
        else if (key == "show_status_bar") config.show_status_bar = (val == "true" || val == "1");

        // Pages, Folders & Touch Gestures
        else if (key == "startup_folder_id") config.startup_folder_id = val;
        else if (key == "remember_last_folder") config.remember_last_folder = (val == "true" || val == "1");
        else if (key == "last_folder_id") config.last_folder_id = val;
        else if (key == "long_press_ms") config.long_press_ms = parseIntSafe(val, 500, 250, 1500);
        else if (key == "double_tap_ms") config.double_tap_ms = parseIntSafe(val, 280, 120, 700);

        // Power, Battery & Display
        else if (key == "idle_enabled") config.idle_enabled = (val == "true" || val == "1");
        else if (key == "brightness_pct") config.brightness_pct = parseIntSafe(val, 100, 10, 100);
        else if (key == "idle_timeout_sec") {
            config.idle_timeout_sec = parseIntSafe(val, 300, 0, 3600);
            if (config.idle_timeout_sec < 60) config.idle_timeout_sec = 300;
        }
        else if (key == "idle_mode") config.idle_mode = static_cast<IdleScreenMode>(parseIntSafe(val, 0, 0, 2));
        else if (key == "low_battery_threshold") config.low_battery_threshold = parseIntSafe(val, 15, 0, 50);
    }

    if (config.client_id.empty()) {
        config.client_id = generateClientId();
        save(config);
    }

    return config;
}

void ConfigManager::save(const AppConfig& config) {
    const std::string path = getConfigPath();
    std::ostringstream file;
    file << "# SwitchDeck Configuration File\n";
    file << "host=" << config.host << "\n";
    file << "port=" << config.port << "\n";
    file << "device_name=" << config.device_name << "\n";
    file << "pairing_code=" << config.pairing_code << "\n";
    file << "device_id=" << config.device_id << "\n";
    file << "device_secret=" << config.device_secret << "\n";
    file << "access_token=" << config.access_token << "\n";
    file << "refresh_cookie=" << config.refresh_cookie << "\n";
    file << "startup_profile_id=" << config.startup_profile_id << "\n";
    file << "client_id=" << config.client_id << "\n";
    file << "auto_reconnect=" << (config.auto_reconnect ? "true" : "false") << "\n";

    file << "\n# Appearance & Grid\n";
    file << "language=" << config.language << "\n";
    file << "theme=" << config.theme << "\n";
    file << "grid_rows_override=" << config.grid_rows_override << "\n";
    file << "grid_cols_override=" << config.grid_cols_override << "\n";
    file << "button_scale_pct=" << config.button_scale_pct << "\n";
    file << "spacing_override=" << config.spacing_override << "\n";
    file << "radius_override=" << config.radius_override << "\n";
    file << "show_empty_cells=" << config.show_empty_cells << "\n";
    file << "show_status_bar=" << (config.show_status_bar ? "true" : "false") << "\n";

    file << "\n# Pages, Folders & Touch Gestures\n";
    file << "startup_folder_id=" << config.startup_folder_id << "\n";
    file << "remember_last_folder=" << (config.remember_last_folder ? "true" : "false") << "\n";
    file << "last_folder_id=" << config.last_folder_id << "\n";
    file << "long_press_ms=" << config.long_press_ms << "\n";
    file << "double_tap_ms=" << config.double_tap_ms << "\n";

    file << "\n# Power, Battery & Display\n";
    file << "idle_enabled=" << (config.idle_enabled ? "true" : "false") << "\n";
    file << "brightness_pct=" << config.brightness_pct << "\n";
    file << "idle_timeout_sec=" << config.idle_timeout_sec << "\n";
    file << "idle_mode=" << static_cast<int>(config.idle_mode) << "\n";
    file << "low_battery_threshold=" << config.low_battery_threshold << "\n";

    const std::string temp = path + ".tmp";
    const std::string backup = path + ".bak";
    const auto contents = file.str();
    auto* pending = std::fopen(temp.c_str(), "wb");
    if (!pending) return;
    bool written = std::fwrite(contents.data(), 1, contents.size(), pending) == contents.size() &&
                   std::fflush(pending) == 0;
#ifdef __SWITCH__
    if (written) written = fsync(fileno(pending)) == 0;
#endif
    if (std::fclose(pending) != 0) written = false;
    if (!written) {
        std::remove(temp.c_str());
        return;
    }

    // FAT can require a free destination. Keep the previous file available across both renames.
    struct stat info{};
    const bool hadConfig = stat(path.c_str(), &info) == 0;
    if ((hadConfig && (!S_ISREG(info.st_mode) ||
                       (std::remove(backup.c_str()) != 0 && errno != ENOENT) ||
                       std::rename(path.c_str(), backup.c_str()) != 0)) ||
        (!hadConfig && errno != ENOENT)) {
        std::remove(temp.c_str());
        return;
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        if (hadConfig) std::rename(backup.c_str(), path.c_str());
        std::remove(temp.c_str());
        return;
    }
#ifdef __SWITCH__
    if (R_FAILED(fsdevCommitDevice("sdmc"))) return;
#endif
    std::remove(backup.c_str());
}
