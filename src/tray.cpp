#include "tray.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>

namespace rtss {
namespace {

GUID iconGuid(std::uint32_t id) {
    return GUID{0x31725400u + id, 0x8d65, 0x49e6, {0x90, 0xc2, 0x49, 0x37, 0x75, 0x42, 0xd6, 0x2a}};
}

NOTIFYICONDATAW notification(HWND window, std::uint32_t id) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = id;
    data.uFlags = NIF_GUID;
    data.guidItem = iconGuid(id);
    return data;
}

}

IconImage renderIconImage(const IconConfig& config, const std::string& text, bool available, float value, int size,
    std::span<const float> history) {
    size = std::clamp(size, 16, 128);
    const int scale = 3;
    const int width = size * scale;
    IconImage image{size, std::vector<std::uint32_t>(static_cast<std::size_t>(size) * size)};
    const auto byte = [](float channel) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255));
    };
    if (!config.transparent) {
        const auto& c = config.backgroundColor;
        std::fill(image.pixels.begin(), image.pixels.end(), 0xff000000u | (byte(c[0]) << 16) | (byte(c[1]) << 8) | byte(c[2]));
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -width;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return image;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        return image;
    }
    HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    auto* mask = static_cast<std::uint32_t*>(bits);
    const auto clear = [&] {
        GdiFlush();
        std::fill_n(mask, static_cast<std::size_t>(width) * width, 0u);
    };
    const auto composite = [&](const std::array<float, 3>& color, float opacity) {
        GdiFlush();
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                unsigned coverage = 0;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        coverage += mask[(y * scale + dy) * width + x * scale + dx] & 0xff;
                    }
                }
                const float alpha = static_cast<float>(coverage) / (255.0f * scale * scale) * opacity;
                auto& pixel = image.pixels[static_cast<std::size_t>(y) * size + x];
                const auto channel = [&](int component, int shift) {
                    return byte(color[component] * alpha + static_cast<float>((pixel >> shift) & 255) / 255 * (1 - alpha));
                };
                const auto a = byte(alpha + static_cast<float>(pixel >> 24) / 255 * (1 - alpha));
                pixel = (a << 24) | (channel(0, 16) << 16) | (channel(1, 8) << 8) | channel(2, 0);
            }
        }
    };
    const auto fill = [&](RECT rect) { FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH))); };
    const auto stroke = [&](const std::vector<POINT>& points, int thickness) {
        if (points.size() < 2) return;
        HPEN pen = CreatePen(PS_SOLID, thickness, RGB(255, 255, 255));
        if (!pen) return;
        HGDIOBJ old = SelectObject(dc, pen);
        Polyline(dc, points.data(), static_cast<int>(points.size()));
        SelectObject(dc, old);
        DeleteObject(pen);
    };
    const bool number = config.style == "number";
    const int padding = scale;
    const int bottom = width - padding;
    const int graphHeight = width - padding * 2;
    const auto fraction = [&](float sample) {
        return std::isfinite(sample) ? std::clamp(sample / config.barMaximum, 0.0f, 1.0f) : 0.0f;
    };
    const auto& graphColor = config.warningThreshold > 0 && value >= config.warningThreshold ? config.warningColor : config.color;
    clear();
    if (!number && available) {
        if (config.style == "bars" || config.style == "line") {
            fill({padding, bottom, width - padding, bottom + scale});
            composite(graphColor, config.graphOpacity * 0.25f);
            clear();
            const int window = std::clamp(config.historySamples, 4, 120);
            if (history.size() > static_cast<std::size_t>(window)) history = history.last(window);
            const int columns = std::min(window, size - 2);
            const int missing = window - static_cast<int>(history.size());
            std::vector<POINT> segment;
            const auto flushSegment = [&] {
                if (segment.size() == 1) fill({segment[0].x - scale / 2, segment[0].y - scale / 2, segment[0].x + scale, segment[0].y + scale});
                else stroke(segment, scale);
                segment.clear();
            };
            for (int column = 0; column < columns; ++column) {
                const int begin = column * window / columns;
                const int end = (column + 1) * window / columns;
                float total = 0;
                int count = 0;
                for (int sample = std::max(begin, missing); sample < end; ++sample) {
                    const float raw = history[sample - missing];
                    if (std::isfinite(raw)) { total += fraction(raw / config.divisor); ++count; }
                }
                if (!count) { flushSegment(); continue; }
                const float level = total / count;
                const int left = padding + column * (width - padding * 2) / columns;
                const int right = padding + (column + 1) * (width - padding * 2) / columns;
                const int top = bottom - static_cast<int>(std::lround(level * graphHeight));
                if (config.style == "bars") {
                    const int gap = !std::isfinite(config.barSpacing) || config.barSpacing < 0
                        ? (columns <= size / 2 ? scale : 1)
                        : static_cast<int>(std::lround((right - left) * std::clamp(config.barSpacing, 0.0f, 0.9f)));
                    fill({left, top, std::max(left + 1, right - gap), bottom});
                } else {
                    segment.push_back({(left + right) / 2, top});
                }
            }
            flushSegment();
        } else if (config.style == "fill") {
            fill({padding, bottom - static_cast<LONG>(std::lround(fraction(value) * graphHeight)), width - padding, bottom});
        } else if (config.style == "ring") {
            const auto arc = [&](float progress) {
                std::vector<POINT> points;
                constexpr float pi = 3.14159265f;
                const float radius = static_cast<float>(width) / 2 - scale * 1.7f;
                const int steps = std::max(1, static_cast<int>(std::ceil(progress * 96)));
                for (int step = 0; step <= steps; ++step) {
                    const float angle = -pi / 2 + 2 * pi * progress * step / steps;
                    points.push_back({static_cast<LONG>(std::lround(width / 2.0f + std::cos(angle) * radius)),
                        static_cast<LONG>(std::lround(width / 2.0f + std::sin(angle) * radius))});
                }
                stroke(points, scale * 2);
            };
            arc(1);
            composite(graphColor, config.graphOpacity * 0.2f);
            clear();
            if (fraction(value) > 0) arc(fraction(value));
        }
        composite(graphColor, config.graphOpacity);
    }
    if (number && config.showBar && available) {
        clear();
        const int height = std::max(scale, width / 10);
        fill({padding, width - height, padding + static_cast<LONG>(std::lround((width - padding * 2) * fraction(value))), width});
        composite(graphColor, 1);
    }
    if (config.showText || !available) {
        clear();
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        const std::wstring wide = utf8ToWide(text);
        const int inset = config.style == "ring" ? scale * 3 : padding;
        const int barHeight = number && config.showBar ? std::max(scale, width / 10) : 0;
        RECT rect{inset, 0, width - inset, width - barHeight - padding};
        int fontHeight = rect.bottom;
        HFONT font = nullptr;
        HGDIOBJ previousFont = nullptr;
        for (;;) {
            font = CreateFontW(-fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");
            if (!font) break;
            HGDIOBJ old = SelectObject(dc, font);
            if (!previousFont) previousFont = old;
            SIZE extent{};
            GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &extent);
            if (extent.cx <= rect.right - rect.left || fontHeight <= 4 * scale) break;
            SelectObject(dc, previousFont);
            DeleteObject(font);
            font = nullptr;
            --fontHeight;
        }
        const UINT flags = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        const float opacity = available ? config.textOpacity : 1.0f;
        if (!number && available) {
            RECT shadow = rect;
            OffsetRect(&shadow, scale / 2, scale / 2);
            DrawTextW(dc, wide.c_str(), static_cast<int>(wide.size()), &shadow, flags);
            composite({0, 0, 0}, opacity * 0.8f);
            clear();
        }
        DrawTextW(dc, wide.c_str(), static_cast<int>(wide.size()), &rect, flags);
        const auto color = !available ? std::array<float, 3>{0.55f, 0.58f, 0.63f} : number ? graphColor : config.textColor;
        composite(color, opacity);
        if (previousFont) SelectObject(dc, previousFont);
        if (font) DeleteObject(font);
    }
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return image;
}

HICON createTrayIcon(const IconImage& image) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.size;
    info.bmiHeader.biHeight = -image.size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) return nullptr;
    std::copy(image.pixels.begin(), image.pixels.end(), static_cast<std::uint32_t*>(bits));
    const int stride = ((image.size + 15) / 16) * 2;
    std::vector<std::byte> maskBits(static_cast<std::size_t>(stride) * image.size);
    HBITMAP mask = CreateBitmap(image.size, image.size, 1, 1, maskBits.data());
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = mask;
    HICON icon = mask ? CreateIconIndirect(&iconInfo) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

TrayManager::~TrayManager() {
    for (auto& [id, entry] : entries_) remove(id, entry);
}

void TrayManager::remove(std::uint32_t id, Entry& entry) {
    if (entry.registered) {
        auto data = notification(window_, id);
        Shell_NotifyIconW(NIM_DELETE, &data);
        entry.registered = false;
    }
    if (entry.icon) DestroyIcon(entry.icon);
    entry.icon = nullptr;
}

int TrayManager::iconSize() const {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const UINT dpi = taskbar ? GetDpiForWindow(taskbar) : GetDpiForSystem();
    return std::max(16, GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : 96));
}

void TrayManager::explorerRestarted() {
    for (auto& [id, entry] : entries_) entry.registered = false;
}

void TrayManager::recordSample(const Config& config, const Snapshot& snapshot) {
    std::set<std::uint32_t> desired;
    for (const auto& icon : config.icons) {
        desired.insert(icon.id);
        auto& history = histories_[icon.id];
        if (history.sensor != icon.sensor) {
            history.sensor = icon.sensor;
            history.values.clear();
        }
        const Sensor* sensor = findSensor(snapshot, icon.sensor);
        history.values.push_back(sensor && sensor->available ? sensor->value : std::numeric_limits<float>::quiet_NaN());
        if (history.values.size() > 120) history.values.erase(history.values.begin());
        ++history.revision;
    }
    std::erase_if(histories_, [&](const auto& entry) { return !desired.contains(entry.first); });
}

std::span<const float> TrayManager::historyFor(const IconConfig& config) const {
    const auto found = histories_.find(config.id);
    if (found == histories_.end() || found->second.sensor != config.sensor) return {};
    return found->second.values;
}

void TrayManager::update(const Config& config, const Snapshot& snapshot) {
    error_.clear();
    std::vector<IconConfig> enabled;
    for (const auto& icon : config.icons) if (icon.enabled) enabled.push_back(icon);
    if (enabled.empty()) {
        IconConfig launcher;
        launcher.id = 65534;
        launcher.label = "RTSSTray settings";
        launcher.showBar = false;
        enabled.push_back(launcher);
    }
    std::set<std::uint32_t> desired;
    const int size = iconSize();
    for (const auto& icon : enabled) {
        desired.insert(icon.id);
        const Sensor* sensor = icon.id == 65534 ? nullptr : findSensor(snapshot, icon.sensor);
        const bool available = sensor && sensor->available;
        const float value = available ? sensor->value / icon.divisor : 0;
        const std::string text = icon.id == 65534 ? "RT" : available ? formatIconValue(value, icon.decimals) : "--";
        std::string tooltip = icon.label + ": ";
        tooltip += available ? std::format("{:.{}f} {}", value, icon.decimals, icon.displayUnit) : "unavailable";
        if (sensor) tooltip += "\n" + sensorTitle(*sensor);
        tooltip += "\n" + snapshot.message + "\nClick: settings / Right-click: menu";
        const std::wstring wideTooltip = utf8ToWide(tooltip);
        Entry& entry = entries_[icon.id];
        const auto history = historyFor(icon);
        const auto found = histories_.find(icon.id);
        const std::uint64_t revision = found != histories_.end() ? found->second.revision : 0;
        const bool historyStyle = icon.style == "bars" || icon.style == "line";
        const bool visualChanged = !entry.icon || entry.config != icon || entry.text != text || entry.size != size
            || entry.available != available || ((icon.showBar || icon.style != "number") && entry.value != value)
            || (historyStyle && entry.historyRevision != revision);
        if (!visualChanged && entry.tooltip == wideTooltip && entry.registered) continue;
        HICON newIcon = visualChanged ? createTrayIcon(renderIconImage(icon, text, available, value, size, history)) : entry.icon;
        if (!newIcon) {
            error_ = "Could not render tray icon";
            continue;
        }
        auto data = notification(window_, icon.id);
        data.uFlags |= NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
        data.uCallbackMessage = trayCallbackMessage;
        data.hIcon = newIcon;
        wcsncpy_s(data.szTip, wideTooltip.c_str(), _TRUNCATE);
        bool registered = entry.registered && Shell_NotifyIconW(NIM_MODIFY, &data);
        if (!registered) {
            registered = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
            if (registered) {
                data.uVersion = NOTIFYICON_VERSION_4;
                Shell_NotifyIconW(NIM_SETVERSION, &data);
            }
        }
        if (visualChanged && entry.icon) DestroyIcon(entry.icon);
        entry.icon = newIcon;
        entry.registered = registered;
        entry.config = icon;
        entry.text = text;
        entry.tooltip = wideTooltip;
        entry.value = value;
        entry.available = available;
        entry.size = size;
        entry.historyRevision = revision;
        if (!registered) error_ = "Explorer did not accept a tray icon; retrying at next refresh";
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!desired.contains(it->first)) {
            remove(it->first, it->second);
            it = entries_.erase(it);
        } else ++it;
    }
}

}
