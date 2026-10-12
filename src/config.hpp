#pragma once

#include "msiab.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace rtss {

struct IconConfig {
    std::uint32_t id = 1;
    bool enabled = true;
    std::string label = "VRAM";
    SensorKey sensor;
    float divisor = 1024.0f;
    int decimals = 1;
    std::string displayUnit = "GB";
    std::array<float, 3> color{0.38f, 0.83f, 1.0f};
    std::array<float, 3> warningColor{1.0f, 0.43f, 0.28f};
    std::array<float, 3> backgroundColor{0.07f, 0.09f, 0.13f};
    bool transparent = false;
    bool showBar = true;
    std::string style = "number";
    bool showText = true;
    std::array<float, 3> textColor{0.96f, 0.97f, 1.0f};
    float textOpacity = 1.0f;
    float graphOpacity = 0.65f;
    int historySamples = 16;
    float barSpacing = -1.0f;
    float barMaximum = 12.0f;
    float warningThreshold = 10.0f;
    bool operator==(const IconConfig&) const = default;
};

struct Config {
    int intervalMs = 1000;
    bool startHidden = false;
    std::vector<IconConfig> icons;
    bool operator==(const Config&) const = default;
};

Config defaultConfig();
void normalizeConfig(Config& config);
std::filesystem::path configFilePath();
Config loadConfig(const std::filesystem::path& path, std::string& error);
bool saveConfig(const std::filesystem::path& path, const Config& config, std::string& error);
std::string formatIconValue(float value, int decimals);

}
