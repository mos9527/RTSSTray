#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rtss {

inline constexpr std::uint32_t globalGpu = 0xffffffffu;
inline constexpr std::uint32_t mahmSignature = 0x4d41484du;

struct SensorKey {
    std::uint32_t sourceId = 0x31;
    std::uint32_t gpu = 0;
    std::string name;
    bool operator==(const SensorKey&) const = default;
};

struct Sensor {
    SensorKey key;
    std::string unit;
    float value = 0;
    float minLimit = 0;
    float maxLimit = 0;
    bool available = false;
};

enum class Connection { waiting, connected, stale, invalid };

struct Snapshot {
    Connection connection = Connection::waiting;
    std::string message = "Waiting for MSI Afterburner";
    std::int32_t pollingTime = 0;
    std::vector<Sensor> sensors;
};

struct MahmHeader {
    std::uint32_t signature;
    std::uint32_t version;
    std::uint32_t headerSize;
    std::uint32_t numEntries;
    std::uint32_t entrySize;
    std::int32_t pollingTime;
    std::uint32_t numGpuEntries;
    std::uint32_t gpuEntrySize;
};

struct MahmEntry {
    char name[260];
    char units[260];
    char localizedName[260];
    char localizedUnits[260];
    char format[260];
    float data;
    float minLimit;
    float maxLimit;
    std::uint32_t flags;
    std::uint32_t gpu;
    std::uint32_t sourceId;
};

static_assert(sizeof(MahmHeader) == 32);
static_assert(sizeof(MahmEntry) == 1324);
static_assert(offsetof(MahmEntry, data) == 1300);

Snapshot parseSharedMemory(std::span<const std::byte> bytes, std::int64_t now);
Snapshot readAfterburner();
const Sensor* findSensor(const Snapshot& snapshot, const SensorKey& key);
std::string sensorTitle(const Sensor& sensor);
std::wstring utf8ToWide(const std::string& text);

}
