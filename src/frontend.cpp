#include "frontend.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <stdexcept>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace rtss {
namespace {

bool editString(const char* label, std::string& value, std::size_t limit = 128) {
    std::vector<char> buffer(limit + 1);
    std::copy_n(value.data(), std::min(value.size(), limit), buffer.data());
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
    value = buffer.data();
    return true;
}

void theme(float scale) {
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

}

Frontend::Frontend(HINSTANCE instance) : instance_(instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = windowProcedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"RTSSTray.Settings";
    if (!RegisterClassExW(&wc)) throw std::runtime_error("Cannot register settings window");
    const UINT dpi = GetDpiForSystem();
    RECT bounds{0, 0, MulDiv(1020, dpi, 96), MulDiv(760, dpi, 96)};
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    window_ = CreateWindowExW(0, wc.lpszClassName, L"RTSSTray - Settings", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, this);
    if (!window_) {
        UnregisterClassW(wc.lpszClassName, instance);
        throw std::runtime_error("Cannot create settings window");
    }
}

Frontend::~Frontend() {
    shutdownRenderer();
    if (window_) DestroyWindow(window_);
    UnregisterClassW(L"RTSSTray.Settings", instance_);
}

bool Frontend::initializeRenderer() {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window_;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &desc,
        swapChain_.ReleaseAndGetAddressOf(), device_.ReleaseAndGetAddressOf(), &level, context_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        swapChain_.Reset();
        device_.Reset();
        context_.Reset();
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &desc,
            swapChain_.ReleaseAndGetAddressOf(), device_.ReleaseAndGetAddressOf(), &level, context_.ReleaseAndGetAddressOf());
    }
    if (FAILED(result) || !createRenderTarget()) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    updateDpi();
    if (!ImGui_ImplWin32_Init(window_)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    initialized_ = true;
    return true;
}

bool Frontend::createRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, renderTarget_.ReleaseAndGetAddressOf()));
}

void Frontend::show() {
    if (closing_) shutdownRenderer();
    closing_ = false;
    if (!initialized_ && !initializeRenderer()) {
        shutdownRenderer();
        throw std::runtime_error("Could not initialize the DirectX 11 settings frontend");
    }
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window_);
}

void Frontend::shutdownRenderer() {
    if (initialized_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        initialized_ = false;
    }
    preview_.Reset();
    previewPixels_.clear();
    renderTarget_.Reset();
    if (context_) context_->ClearState();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
    resizeWidth_ = resizeHeight_ = 0;
    occluded_ = false;
}

bool Frontend::visible() const {
    return initialized_ && !closing_ && IsWindowVisible(window_) && !IsIconic(window_);
}

void Frontend::updateDpi() {
    const UINT dpi = GetDpiForWindow(window_);
    scale_ = static_cast<float>(dpi ? dpi : 96) / 96;
    if (ImGui::GetCurrentContext()) theme(scale_);
}

LRESULT CALLBACK Frontend::windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<Frontend*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Frontend*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Frontend::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (initialized_ && ImGui_ImplWin32_WndProcHandler(window_, message, wParam, lParam)) return TRUE;
    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            resizeWidth_ = LOWORD(lParam);
            resizeHeight_ = HIWORD(lParam);
        }
        return 0;
    case WM_DPICHANGED: {
        updateDpi();
        const auto* rect = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(window_);
        limits->ptMinTrackSize = {MulDiv(760, dpi ? dpi : 96, 96), MulDiv(650, dpi ? dpi : 96, 96)};
        return 0;
    }
    case WM_CLOSE:
        closing_ = true;
        ShowWindow(window_, SW_HIDE);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

void Frontend::drawPreview(const IconConfig& config, const Snapshot& snapshot) {
    const Sensor* sensor = findSensor(snapshot, config.sensor);
    const bool available = sensor && sensor->available;
    const float value = available ? sensor->value / config.divisor : 0;
    const std::string text = available ? formatIconValue(value, config.decimals) : "--";
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const UINT dpi = taskbar ? GetDpiForWindow(taskbar) : GetDpiForSystem();
    const int size = std::max(16, GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : 96));
    const auto history = tray_ ? tray_->historyFor(config) : std::span<const float>{};
    auto image = renderIconImage(config, text, available, value, size, history);
    if (!preview_ || image.pixels != previewPixels_) {
        previewPixels_ = image.pixels;
        for (auto& pixel : image.pixels) {
            const std::uint32_t alpha = pixel >> 24;
            if (alpha && alpha != 255) {
                const auto unpremultiply = [&](int shift) { return std::min(255u, ((pixel >> shift) & 255u) * 255 / alpha); };
                pixel = (alpha << 24) | (unpremultiply(16) << 16) | (unpremultiply(8) << 8) | unpremultiply(0);
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = size;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = image.pixels.data();
        data.SysMemPitch = size * sizeof(std::uint32_t);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        preview_.Reset();
        if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, texture.GetAddressOf()))) {
            device_->CreateShaderResourceView(texture.Get(), nullptr, preview_.GetAddressOf());
        }
    }
    if (preview_) {
        const auto texture = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(preview_.Get()));
        ImGui::Image(texture, ImVec2(static_cast<float>(size), static_cast<float>(size)));
        ImGui::SameLine();
        ImGui::Image(texture, ImVec2(64 * scale_, 64 * scale_));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s %s", text.c_str(), config.displayUnit.c_str());
        ImGui::TextDisabled("%d x %d native / enlarged", size, size);
        ImGui::TextDisabled(available ? "Live MSIAB sample" : "No sample: showing unavailable state");
        ImGui::EndGroup();
    }
}

UiActions Frontend::drawSettings(Config& config, const Config& saved, const Snapshot& snapshot,
    const std::string& notice, const std::filesystem::path& path, const std::string& trayError) {
    UiActions actions;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("RTSSTray", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.38f, 0.83f, 1, 1), "RTSSTray");
    const bool connected = snapshot.connection == Connection::connected;
    ImGui::TextColored(connected ? ImVec4(0.52f, 0.89f, 0.62f, 1) : ImVec4(0.96f, 0.76f, 0.40f, 1), "%s", snapshot.message.c_str());
    if (!connected) ImGui::TextWrapped("Start MSI Afterburner and enable the required hardware monitoring items. OSD output is not required.");
    if (!trayError.empty()) ImGui::TextWrapped("%s", trayError.c_str());
    if (!notice.empty()) ImGui::TextWrapped("%s", notice.c_str());
    ImGui::Separator();

    ImGui::SetNextItemWidth(180 * scale_);
    ImGui::SliderInt("Polling interval (ms)", &config.intervalMs, 250, 10000);
    ImGui::SameLine();
    ImGui::Checkbox("Start in tray", &config.startHidden);
    ImGui::TextDisabled("Closing settings keeps monitoring active. Hidden settings release their DX11 device.");

    if (ImGui::BeginTabBar("Pages")) {
        if (ImGui::BeginTabItem("Tray icons")) {
            const float childHeight = std::max(260 * scale_, ImGui::GetContentRegionAvail().y - 106 * scale_);
            ImGui::BeginChild("Icon list", ImVec2(220 * scale_, childHeight), ImGuiChildFlags_Borders);
            for (auto& icon : config.icons) {
                ImGui::PushID(static_cast<int>(icon.id));
                ImGui::Checkbox("##enabled", &icon.enabled);
                ImGui::SameLine();
                if (ImGui::Selectable(icon.label.c_str(), selectedId_ == icon.id)) selectedId_ = icon.id;
                ImGui::PopID();
            }
            ImGui::Spacing();
            if (config.icons.size() < 12 && ImGui::Button("+ Add icon", ImVec2(-1, 0))) {
                IconConfig icon;
                icon.label = "New metric";
                icon.id = 1;
                for (const auto& current : config.icons) icon.id = std::max(icon.id, current.id + 1);
                selectedId_ = icon.id;
                config.icons.push_back(icon);
            }
            ImGui::TextWrapped("Up to 12 icons. Windows controls their visibility and ordering.");
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("Icon editor", ImVec2(0, childHeight), ImGuiChildFlags_Borders);
            auto selected = std::find_if(config.icons.begin(), config.icons.end(), [&](const auto& icon) { return icon.id == selectedId_; });
            if (selected == config.icons.end() && !config.icons.empty()) {
                selected = config.icons.begin();
                selectedId_ = selected->id;
            }
            if (selected != config.icons.end()) {
                auto& icon = *selected;
                ImGui::TextDisabled("LIVE ICON PREVIEW");
                drawPreview(icon, snapshot);
                const char* styleKeys[] = {"number", "bars", "line", "fill", "ring"};
                const char* styleNames[] = {"Number + optional bottom bar", "History bars", "History line", "Vertical fill", "Progress ring"};
                int styleIndex = 0;
                for (int index = 0; index < 5; ++index) if (icon.style == styleKeys[index]) styleIndex = index;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##IconStyle", &styleIndex, styleNames, 5)) icon.style = styleKeys[styleIndex];
                const bool graphStyle = icon.style != "number";
                if (graphStyle) {
                    ImGui::Checkbox("Overlay value text", &icon.showText);
                    ImGui::TextDisabled("Graph behind / text in front, blended independently.");
                    ImGui::SetNextItemWidth(150 * scale_);
                    ImGui::SliderFloat("Graph opacity", &icon.graphOpacity, 0, 1, "%.2f");
                    if (icon.showText) ImGui::ColorEdit3("Overlay text", icon.textColor.data(), ImGuiColorEditFlags_NoInputs);
                }
                if (icon.showText || !graphStyle) {
                    ImGui::SetNextItemWidth(150 * scale_);
                    ImGui::SliderFloat("Text opacity", &icon.textOpacity, 0, 1, "%.2f");
                }
                if (icon.style == "bars" || icon.style == "line") {
                    ImGui::SetNextItemWidth(150 * scale_);
                    ImGui::SliderInt("History samples", &icon.historySamples, 4, 120);
                    ImGui::TextDisabled("%.1f seconds; newest at right; missing data leaves gaps.", icon.historySamples * config.intervalMs / 1000.0f);
                }
                if (icon.style == "bars") {
                    bool automaticSpacing = icon.barSpacing < 0;
                    if (ImGui::Checkbox("Automatic bar spacing", &automaticSpacing)) icon.barSpacing = automaticSpacing ? -1.0f : 0.25f;
                    if (!automaticSpacing) {
                        float spacingPercent = icon.barSpacing * 100;
                        ImGui::SetNextItemWidth(150 * scale_);
                        if (ImGui::SliderFloat("Bar spacing", &spacingPercent, 0, 90, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) icon.barSpacing = spacingPercent / 100;
                        ImGui::TextDisabled("Percent of each column width; 0%% = no gap.");
                    }
                }
                ImGui::Separator();
                ImGui::SetNextItemWidth(-1);
                editString("##Label", icon.label, 96);
                const Sensor* sensor = findSensor(snapshot, icon.sensor);
                const std::string current = sensor ? sensorTitle(*sensor)
                    : std::format("Unavailable / source 0x{:X} / GPU {}", icon.sensor.sourceId, icon.sensor.gpu == globalGpu ? "System" : std::to_string(icon.sensor.gpu + 1));
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##Sensor", current.c_str())) {
                    for (std::size_t index = 0; index < snapshot.sensors.size(); ++index) {
                        const auto& candidate = snapshot.sensors[index];
                        ImGui::PushID(static_cast<int>(index));
                        if (ImGui::Selectable(sensorTitle(candidate).c_str(), icon.sensor == candidate.key)) {
                            icon.sensor = candidate.key;
                            icon.displayUnit = candidate.unit;
                            icon.divisor = 1;
                            icon.decimals = 0;
                            if (std::isfinite(candidate.maxLimit) && candidate.maxLimit > 0) {
                                icon.barMaximum = candidate.maxLimit;
                                icon.warningThreshold = candidate.maxLimit * 0.9f;
                            }
                        }
                        ImGui::PopID();
                    }
                    if (snapshot.sensors.empty()) ImGui::TextDisabled("No sensors published yet");
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("Displayed value = sensor value / divisor");
                ImGui::SetNextItemWidth(130 * scale_);
                ImGui::InputFloat("Divisor", &icon.divisor, 0, 0, "%.3f");
                ImGui::SameLine();
                if (ImGui::Button("Raw")) icon.divisor = 1;
                ImGui::SameLine();
                if (ImGui::Button("MB -> GB")) { icon.divisor = 1024; icon.displayUnit = "GB"; icon.decimals = 1; }
                ImGui::SetNextItemWidth(130 * scale_);
                ImGui::SliderInt("Decimal places", &icon.decimals, 0, 2);
                ImGui::SetNextItemWidth(130 * scale_);
                editString("Display unit", icon.displayUnit, 24);
                ImGui::ColorEdit3(graphStyle ? "Graph color" : "Text / bar", icon.color.data(), ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                ImGui::ColorEdit3("Warning", icon.warningColor.data(), ImGuiColorEditFlags_NoInputs);
                ImGui::Checkbox("Transparent background", &icon.transparent);
                if (!icon.transparent) ImGui::ColorEdit3("Background", icon.backgroundColor.data(), ImGuiColorEditFlags_NoInputs);
                if (!graphStyle) ImGui::Checkbox("Occupancy bar", &icon.showBar);
                if (graphStyle || icon.showBar) {
                    ImGui::SetNextItemWidth(130 * scale_);
                    ImGui::InputFloat("Graph full scale", &icon.barMaximum);
                }
                ImGui::SetNextItemWidth(130 * scale_);
                ImGui::InputFloat("Warning above (0 = off)", &icon.warningThreshold);
                ImGui::TextDisabled("Scale and warning threshold use displayed units.");
                ImGui::Spacing();
                if (ImGui::Button("Remove this icon")) {
                    config.icons.erase(selected);
                    selectedId_ = config.icons.empty() ? 0 : config.icons.front().id;
                }
            } else {
                ImGui::TextWrapped("Add a metric to begin. With all icons disabled, an RT launcher remains available.");
            }
            ImGui::EndChild();
            ImGui::TextWrapped("Changes are applied to the tray immediately. Save persists them; Revert restores the last saved configuration.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sensor browser")) {
            static char filter[128]{};
            ImGui::SetNextItemWidth(300 * scale_);
            ImGui::InputTextWithHint("##Filter", "Filter sensor name...", filter, sizeof(filter));
            const float height = std::max(180 * scale_, ImGui::GetContentRegionAvail().y - 85 * scale_);
            if (ImGui::BeginTable("Sensors", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, height))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Sensor", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Device");
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupColumn("Unit");
                ImGui::TableSetupColumn("Source ID");
                ImGui::TableHeadersRow();
                for (const auto& sensor : snapshot.sensors) {
                    if (filter[0] && sensor.key.name.find(filter) == std::string::npos) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(sensor.key.name.c_str());
                    ImGui::TableNextColumn();
                    if (sensor.key.gpu == globalGpu) ImGui::TextUnformatted("System");
                    else ImGui::Text("GPU %u", sensor.key.gpu + 1);
                    ImGui::TableNextColumn();
                    if (sensor.available) ImGui::Text("%.2f", sensor.value);
                    else ImGui::TextDisabled("--");
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(sensor.unit.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("0x%X", sensor.key.sourceId);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(100 * scale_, 0))) actions.save = true;
    ImGui::SameLine();
    if (ImGui::Button("Revert")) actions.revert = true;
    ImGui::SameLine();
    if (ImGui::Button("Defaults")) actions.defaults = true;
    ImGui::SameLine();
    if (ImGui::Button("Hide to tray")) {
        closing_ = true;
        ShowWindow(window_, SW_HIDE);
    }
    ImGui::SameLine();
    ImGui::TextDisabled(config == saved ? "Saved" : "Unsaved changes");
    const auto encoded = path.u8string();
    const std::string displayPath(encoded.begin(), encoded.end());
    ImGui::TextDisabled("%s", displayPath.c_str());
    ImGui::End();
    return actions;
}

UiActions Frontend::render(Config& config, const Config& saved, const Snapshot& snapshot,
    const std::string& notice, const std::filesystem::path& path, const std::string& trayError) {
    if (!visible()) return {};
    if (occluded_ && swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) return {};
    occluded_ = false;
    if (resizeWidth_ && resizeHeight_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        renderTarget_.Reset();
        if (FAILED(swapChain_->ResizeBuffers(0, resizeWidth_, resizeHeight_, DXGI_FORMAT_UNKNOWN, 0)) || !createRenderTarget()) {
            throw std::runtime_error("Could not resize DirectX 11 settings surface");
        }
        resizeWidth_ = resizeHeight_ = 0;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const UiActions actions = drawSettings(config, saved, snapshot, notice, path, trayError);
    ImGui::Render();
    const float clear[] = {0.055f, 0.067f, 0.09f, 1};
    ID3D11RenderTargetView* target = renderTarget_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->ClearRenderTargetView(target, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT result = swapChain_->Present(1, 0);
    occluded_ = result == DXGI_STATUS_OCCLUDED;
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        shutdownRenderer();
        if (!initializeRenderer()) throw std::runtime_error("DirectX 11 device recovery failed");
    } else if (FAILED(result)) throw std::runtime_error("DirectX 11 presentation failed");
    return actions;
}

}
