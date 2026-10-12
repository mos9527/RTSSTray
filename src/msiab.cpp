#include "msiab.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>

namespace rtss {
namespace {

std::string boundedString(const char* data, std::size_t size) {
    return {data, std::find(data, data + size, '\0')};
}

std::string ansiToUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length) return {};
    std::wstring wide(length, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, result.data(), bytes, nullptr, nullptr);
    return result;
}

Snapshot invalid(const char* message) {
    Snapshot snapshot;
    snapshot.connection = Connection::invalid;
    snapshot.message = message;
    return snapshot;
}

}

Snapshot parseSharedMemory(std::span<const std::byte> bytes, std::int64_t now) {
    if (bytes.size() < sizeof(MahmHeader)) return invalid("Truncated MSIAB header");
    MahmHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.signature != mahmSignature) return invalid("MSIAB shared memory is not initialized");
    if ((header.version >> 16) != 2) return invalid("Unsupported MSIAB shared memory version");
    if (header.headerSize < sizeof(MahmHeader) || header.headerSize > bytes.size()
        || header.entrySize < sizeof(MahmEntry) || header.numEntries > 16384) {
        return invalid("Invalid MSIAB shared memory layout");
    }
    if (header.numEntries > (bytes.size() - header.headerSize) / header.entrySize) {
        return invalid("MSIAB sensor array exceeds mapped memory");
    }
    const std::size_t gpuOffset = header.headerSize + static_cast<std::size_t>(header.numEntries) * header.entrySize;
    if (header.numGpuEntries && (header.gpuEntrySize < 1304
        || header.numGpuEntries > (bytes.size() - gpuOffset) / header.gpuEntrySize)) {
        return invalid("Invalid MSIAB GPU array");
    }

    Snapshot snapshot;
    snapshot.pollingTime = header.pollingTime;
    const bool stale = header.pollingTime <= 0 || now - header.pollingTime > 15 || header.pollingTime - now > 15;
    snapshot.connection = stale ? Connection::stale : Connection::connected;
    snapshot.message = stale ? "MSIAB data is stale (no recent polling)" : "Connected to MSI Afterburner";
    snapshot.sensors.reserve(header.numEntries);
    for (std::uint32_t index = 0; index < header.numEntries; ++index) {
        MahmEntry entry{};
        std::memcpy(&entry, bytes.data() + header.headerSize + static_cast<std::size_t>(index) * header.entrySize, sizeof(entry));
        Sensor sensor;
        sensor.key = {entry.sourceId, entry.gpu, ansiToUtf8(boundedString(entry.name, sizeof(entry.name)))};
        if (entry.sourceId == 0x91 || entry.sourceId == 0x92 || entry.sourceId == 0x93) {
            sensor.key.gpu = globalGpu;
        }
        sensor.unit = ansiToUtf8(boundedString(entry.units, sizeof(entry.units)));
        sensor.value = entry.data;
        sensor.minLimit = entry.minLimit;
        sensor.maxLimit = entry.maxLimit;
        sensor.available = !stale && std::isfinite(entry.data) && entry.data != std::numeric_limits<float>::max();
        snapshot.sensors.push_back(std::move(sensor));
    }
    return snapshot;
}

Snapshot readAfterburner() {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"MAHMSharedMemory");
    if (!mapping) {
        Snapshot snapshot;
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            snapshot.message = "MSIAB access denied; check application permissions";
        }
        return snapshot;
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        return invalid("Could not map MSIAB shared memory");
    }
    MEMORY_BASIC_INFORMATION info{};
    Snapshot snapshot;
    if (VirtualQuery(view, &info, sizeof(info)) && info.RegionSize <= 64 * 1024 * 1024) {
        const auto* data = static_cast<const std::byte*>(view);
        std::vector<std::byte> copy(info.RegionSize);
        std::memcpy(copy.data(), data, copy.size());
        snapshot = parseSharedMemory(copy, static_cast<std::int64_t>(std::time(nullptr)));
    } else {
        snapshot = invalid("Unexpected MSIAB mapping size");
    }
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return snapshot;
}

const Sensor* findSensor(const Snapshot& snapshot, const SensorKey& key) {
    const auto matches = [&](const Sensor& sensor) {
        return sensor.key.sourceId == key.sourceId && sensor.key.gpu == key.gpu
            && (key.name.empty() || sensor.key.name == key.name);
    };
    const auto found = std::find_if(snapshot.sensors.begin(), snapshot.sensors.end(), matches);
    return found != snapshot.sensors.end() ? &*found : nullptr;
}

std::string sensorTitle(const Sensor& sensor) {
    const std::string device = sensor.key.gpu == globalGpu ? "System" : "GPU " + std::to_string(sensor.key.gpu + 1);
    return device + " / " + sensor.key.name + " [" + sensor.unit + "]";
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

}
