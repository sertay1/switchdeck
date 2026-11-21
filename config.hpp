#pragma once
#include <string>

enum class IdleScreenMode { Dim = 0, OledBlack = 1, Sleep = 2 };

struct AppConfig {
    // Connection & Authentication
    std::string host = "192.168.1.43";
    int port = 8193;
    std::string device_name = "SwitchDeck";
    std::string pairing_code = "";
    std::string device_id = "";
    std::string device_secret = "";
    std::string access_token = "";
    std::string refresh_cookie = "";
    std::string startup_profile_id = "";
    std::string client_id = "";
    bool auto_reconnect = true;

    // Appearance & Grid Settings
    int language = 0;               // 0 = Turkce (TR), 1 = English (EN)
    int theme = 0;                  // 0 = Koyu (Dark), 1 = Acik (Light), 2 = OLED Tam Siyah
    int grid_rows_override = 0;     // 0 = Otomatik (PC), 1..16
    int grid_cols_override = 0;     // 0 = Otomatik (PC), 3..16
    int button_scale_pct = 100;     // 70..115 (%)
    int spacing_override = -1;      // -1 = Otomatik (PC), 0..40 px
    int radius_override = -1;       // -1 = Otomatik (PC), 0..60 px
    int show_empty_cells = -1;      // -1 = Otomatik (PC), 0 = Gizle, 1 = Goster
    bool show_status_bar = true;    // Top status bar with Brand, Folder, Connection, Ping & Battery

    // Pages, Folders & Touch Gestures
    std::string startup_folder_id = ""; // Empty = Profile default folder
    bool remember_last_folder = true;
    std::string last_folder_id = "";
    int long_press_ms = 500;        // 300..1000 ms
    int double_tap_ms = 280;        // 150..500 ms

    // Power, Battery & Display
    bool idle_enabled = true;       // Apply the selected idle screen mode after the timeout
    int brightness_pct = 100;       // 10..100 % (hardware backlight via libnx lbl)
    int idle_timeout_sec = 300;     // 60, 180, 300, 600, 900, 1800, 3600
    IdleScreenMode idle_mode = IdleScreenMode::Dim;
    int low_battery_threshold = 15; // 0 = Off, 10, 15, 20, 30 (%)

    bool sameServer(const AppConfig& other) const {
        return host == other.host && port == other.port;
    }

    void copySessionFrom(const AppConfig& other) {
        device_id = other.device_id;
        device_secret = other.device_secret;
        access_token = other.access_token;
        refresh_cookie = other.refresh_cookie;
    }
};

class ConfigManager {
public:
    static AppConfig load();
    static void save(const AppConfig& config);
private:
    static std::string getConfigPath();
    static std::string generateClientId();
};
