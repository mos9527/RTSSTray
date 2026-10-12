#pragma once

#include "tray.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <memory>

namespace rtss {

struct UiActions {
    bool save = false;
    bool revert = false;
    bool defaults = false;
};

class Frontend {
public:
    explicit Frontend(HINSTANCE instance);
    ~Frontend();
    void show();
    void shutdownRenderer();
    bool visible() const;
    bool closing() const { return closing_; }
    UiActions render(Config& config, const Config& saved, const Snapshot& snapshot,
        const std::string& notice, const std::filesystem::path& path, const std::string& trayError);
    HWND window() const { return window_; }
    void setTray(const TrayManager& tray) { tray_ = &tray; }

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool initializeRenderer();
    bool createRenderTarget();
    void updateDpi();
    void drawPreview(const IconConfig& config, const Snapshot& snapshot);
    UiActions drawSettings(Config& config, const Config& saved, const Snapshot& snapshot,
        const std::string& notice, const std::filesystem::path& path, const std::string& trayError);

    HWND window_ = nullptr;
    const TrayManager* tray_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool initialized_ = false;
    bool closing_ = false;
    bool showDemo_ = false;
    bool occluded_ = false;
    UINT resizeWidth_ = 0;
    UINT resizeHeight_ = 0;
    std::uint32_t selectedId_ = 1;
    float scale_ = 1;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preview_;
    std::vector<std::uint32_t> previewPixels_;
};

}
