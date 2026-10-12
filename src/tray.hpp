#pragma once

#include "config.hpp"

#include <windows.h>
#include <map>

namespace rtss {

inline constexpr UINT trayCallbackMessage = WM_APP + 1;
inline constexpr UINT showSettingsMessage = WM_APP + 2;

struct IconImage {
    int size = 0;
    std::vector<std::uint32_t> pixels;
};

IconImage renderIconImage(const IconConfig& config, const std::string& text, bool available, float value, int size,
    std::span<const float> history = {});
HICON createTrayIcon(const IconImage& image);

class TrayManager {
public:
    explicit TrayManager(HWND window) : window_(window) {}
    ~TrayManager();
    void update(const Config& config, const Snapshot& snapshot);
    void recordSample(const Config& config, const Snapshot& snapshot);
    std::span<const float> historyFor(const IconConfig& config) const;
    void explorerRestarted();
    const std::string& error() const { return error_; }
    int iconSize() const;

private:
    struct Entry {
        HICON icon = nullptr;
        bool registered = false;
        IconConfig config;
        std::string text;
        std::wstring tooltip;
        float value = 0;
        bool available = false;
        int size = 0;
        std::uint64_t historyRevision = 0;
    };
    void remove(std::uint32_t id, Entry& entry);
    struct History {
        SensorKey sensor;
        std::vector<float> values;
        std::uint64_t revision = 0;
    };
    HWND window_;
    std::map<std::uint32_t, Entry> entries_;
    std::map<std::uint32_t, History> histories_;
    std::string error_;
};

}
