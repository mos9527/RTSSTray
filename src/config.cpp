#include "config.hpp"

#include <windows.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <set>

namespace rtss {
using nlohmann::json;

void to_json(json& j, const SensorKey& key) {
    j = json{{"sourceId", key.sourceId}, {"gpu", key.gpu}, {"name", key.name}};
}
void from_json(const json& j, SensorKey& key) {
    key.sourceId = j.value("sourceId", key.sourceId);
    key.gpu = j.value("gpu", key.gpu);
    key.name = j.value("name", key.name);
}
void to_json(json& j, const IconConfig& icon) {
    j = json{{"id", icon.id}, {"enabled", icon.enabled}, {"label", icon.label}, {"sensor", icon.sensor},
        {"divisor", icon.divisor}, {"decimals", icon.decimals}, {"displayUnit", icon.displayUnit},
        {"color", icon.color}, {"warningColor", icon.warningColor}, {"backgroundColor", icon.backgroundColor},
        {"transparent", icon.transparent}, {"showBar", icon.showBar},
        {"style", icon.style}, {"showText", icon.showText}, {"textColor", icon.textColor},
        {"textOpacity", icon.textOpacity}, {"graphOpacity", icon.graphOpacity}, {"historySamples", icon.historySamples},
        {"barSpacing", icon.barSpacing}, {"barMaximum", icon.barMaximum}, {"warningThreshold", icon.warningThreshold}};
}
void from_json(const json& j, IconConfig& icon) {
    icon.id = j.value("id", icon.id);
    icon.enabled = j.value("enabled", icon.enabled);
    icon.label = j.value("label", icon.label);
    icon.sensor = j.value("sensor", icon.sensor);
    icon.divisor = j.value("divisor", icon.divisor);
    icon.decimals = j.value("decimals", icon.decimals);
    icon.displayUnit = j.value("displayUnit", icon.displayUnit);
    icon.color = j.value("color", icon.color);
    icon.warningColor = j.value("warningColor", icon.warningColor);
    icon.backgroundColor = j.value("backgroundColor", icon.backgroundColor);
    icon.transparent = j.value("transparent", icon.transparent);
    icon.showBar = j.value("showBar", icon.showBar);
    icon.style = j.value("style", icon.style);
    icon.showText = j.value("showText", icon.showText);
    icon.textColor = j.value("textColor", icon.textColor);
    icon.textOpacity = j.value("textOpacity", icon.textOpacity);
    icon.graphOpacity = j.value("graphOpacity", icon.graphOpacity);
    icon.historySamples = j.value("historySamples", icon.historySamples);
    icon.barSpacing = j.value("barSpacing", icon.barSpacing);
    icon.barMaximum = j.value("barMaximum", icon.barMaximum);
    icon.warningThreshold = j.value("warningThreshold", icon.warningThreshold);
}

Config defaultConfig() {
    Config config;
    IconConfig vram;
    config.icons.push_back(vram);
    IconConfig ram;
    ram.id = 2;
    ram.label = "RAM";
    ram.sensor = {0x91, globalGpu, {}};
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    ram.barMaximum = GlobalMemoryStatusEx(&memory)
        ? static_cast<float>(memory.ullTotalPhys) / (1024.0f * 1024.0f * 1024.0f) : 32.0f;
    ram.warningThreshold = ram.barMaximum * 0.9f;
    ram.color = {0.52f, 0.89f, 0.62f};
    config.icons.push_back(ram);
    IconConfig power;
    power.id = 3;
    power.label = "GPU power";
    power.sensor = {0x61, 0, {}};
    power.divisor = 1;
    power.decimals = 0;
    power.displayUnit = "W";
    power.barMaximum = 300;
    power.warningThreshold = 250;
    power.color = {0.96f, 0.76f, 0.40f};
    config.icons.push_back(power);
    return config;
}

void normalizeConfig(Config& config) {
    config.intervalMs = std::clamp(config.intervalMs, 250, 10000);
    if (config.icons.size() > 12) config.icons.resize(12);
    std::set<std::uint32_t> ids;
    for (auto& icon : config.icons) {
        if (!icon.id || icon.id >= 65535 || ids.contains(icon.id)) {
            icon.id = 1;
            while (ids.contains(icon.id)) ++icon.id;
        }
        ids.insert(icon.id);
        icon.decimals = std::clamp(icon.decimals, 0, 2);
        if (!std::isfinite(icon.divisor) || icon.divisor <= 0) icon.divisor = 1;
        if (!std::isfinite(icon.barMaximum) || icon.barMaximum <= 0) icon.barMaximum = 100;
        if (!std::isfinite(icon.warningThreshold)) icon.warningThreshold = 0;
        if (icon.style != "number" && icon.style != "bars" && icon.style != "line" && icon.style != "fill" && icon.style != "ring") icon.style = "number";
        if (icon.style == "number") icon.showText = true;
        icon.historySamples = std::clamp(icon.historySamples, 4, 120);
        icon.barSpacing = !std::isfinite(icon.barSpacing) || icon.barSpacing < 0 ? -1.0f : std::clamp(icon.barSpacing, 0.0f, 0.9f);
        for (auto* opacity : {&icon.textOpacity, &icon.graphOpacity}) {
            *opacity = std::isfinite(*opacity) ? std::clamp(*opacity, 0.0f, 1.0f) : 1.0f;
        }
        for (auto* color : {&icon.color, &icon.warningColor, &icon.backgroundColor, &icon.textColor}) {
            for (float& channel : *color) channel = std::isfinite(channel) ? std::clamp(channel, 0.0f, 1.0f) : 0.0f;
        }
        if (icon.label.size() > 96) icon.label.resize(96);
        if (icon.displayUnit.size() > 24) icon.displayUnit.resize(24);
    }
}

std::filesystem::path configFilePath() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
        throw std::runtime_error("Cannot locate LocalAppData");
    }
    std::filesystem::path path(local);
    CoTaskMemFree(local);
    return path / L"RTSSTray" / L"config.json";
}

Config loadConfig(const std::filesystem::path& path, std::string& error) {
    error.clear();
    try {
        if (!std::filesystem::exists(path)) return defaultConfig();
        if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Configuration exceeds 1 MB");
        std::ifstream stream(path);
        if (!stream) throw std::runtime_error("Cannot open configuration");
        json j;
        stream >> j;
        if (j.value("version", 1) != 1) throw std::runtime_error("Unsupported configuration version");
        Config config;
        config.intervalMs = j.value("intervalMs", config.intervalMs);
        config.startHidden = j.value("startHidden", config.startHidden);
        config.icons = j.value("icons", defaultConfig().icons);
        normalizeConfig(config);
        return config;
    } catch (const std::exception& ex) {
        error = ex.what();
        return defaultConfig();
    }
}

bool saveConfig(const std::filesystem::path& path, const Config& config, std::string& error) {
    error.clear();
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    try {
        std::filesystem::create_directories(path.parent_path());
        Config normalized = config;
        normalizeConfig(normalized);
        const json j{{"version", 1}, {"intervalMs", normalized.intervalMs},
            {"startHidden", normalized.startHidden}, {"icons", normalized.icons}};
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream << j.dump(2) << '\n';
        stream.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Cannot replace configuration file (Win32 " + std::to_string(GetLastError()) + ")");
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
}

std::string formatIconValue(float value, int decimals) {
    if (!std::isfinite(value)) return "--";
    if (std::abs(value) >= 10000) return value < 0 ? "-HI" : "HI";
    std::string result = std::format("{:.{}f}", value, std::clamp(decimals, 0, 2));
    if (result.size() > 4) result = std::format("{:.0f}", value);
    return result;
}

}
