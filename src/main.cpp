#include "frontend.hpp"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>

namespace rtss {
namespace {

using Clock = std::chrono::steady_clock;

struct Controller {
    Frontend* frontend = nullptr;
    TrayManager* tray = nullptr;
    bool exitRequested = false;
    bool refreshRequested = false;
    UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
};

LRESULT CALLBACK controllerProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* controller = reinterpret_cast<Controller*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        controller = static_cast<Controller*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(controller));
    }
    if (!controller) return DefWindowProcW(window, message, wParam, lParam);
    if (message == controller->taskbarCreated) {
        if (controller->tray) controller->tray->explorerRestarted();
        controller->refreshRequested = true;
        return 0;
    }
    if (message == WM_DISPLAYCHANGE || message == WM_SETTINGCHANGE || message == WM_DPICHANGED) {
        controller->refreshRequested = true;
    }
    if (message == showSettingsMessage) {
        if (controller->frontend) {
            try { controller->frontend->show(); }
            catch (const std::exception& ex) { MessageBoxW(window, utf8ToWide(ex.what()).c_str(), L"RTSSTray", MB_OK | MB_ICONERROR); }
        }
        return 0;
    }
    if (message == trayCallbackMessage) {
        const UINT event = LOWORD(lParam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
            PostMessageW(window, showSettingsMessage, 0, 0);
        } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            POINT cursor{};
            GetCursorPos(&cursor);
            HMENU menu = CreatePopupMenu();
            if (menu) {
                AppendMenuW(menu, MF_STRING, 1, L"Settings...");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 2, L"Exit RTSSTray");
                SetForegroundWindow(window);
                const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    cursor.x, cursor.y, 0, window, nullptr);
                DestroyMenu(menu);
                PostMessageW(window, WM_NULL, 0, 0);
                if (command == 1) PostMessageW(window, showSettingsMessage, 0, 0);
                if (command == 2) controller->exitRequested = true;
            }
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        controller->exitRequested = true;
        return 0;
    }
    if (message == WM_QUERYENDSESSION) return TRUE;
    if (message == WM_ENDSESSION && wParam) PostQuitMessage(0);
    return DefWindowProcW(window, message, wParam, lParam);
}

class ControllerWindow {
public:
    ControllerWindow(HINSTANCE instance, Controller& controller) : instance_(instance) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = controllerProcedure;
        wc.hInstance = instance;
        wc.lpszClassName = L"RTSSTray.Controller";
        if (!RegisterClassExW(&wc)) throw std::runtime_error("Cannot register tray controller");
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"RTSSTray", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, instance, &controller);
        if (!window_) {
            UnregisterClassW(wc.lpszClassName, instance);
            throw std::runtime_error("Cannot create tray controller");
        }
    }
    ~ControllerWindow() {
        DestroyWindow(window_);
        UnregisterClassW(L"RTSSTray.Controller", instance_);
    }
    HWND get() const { return window_; }
private:
    HINSTANCE instance_;
    HWND window_ = nullptr;
};

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { if (handle_) CloseHandle(handle_); }
private:
    HANDLE handle_;
};

int run(HINSTANCE instance) {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    bool background = false;
    for (int i = 1; arguments && i < argumentCount; ++i) {
        if (std::wstring_view(arguments[i]) == L"--background") background = true;
    }
    if (arguments) LocalFree(arguments);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\RTSSTray.Singleton");
    const DWORD mutexError = GetLastError();
    UniqueHandle singleton(mutex);
    if (!mutex) throw std::runtime_error("Cannot create single-instance mutex");
    if (mutexError == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"RTSSTray.Controller", nullptr);
        if (existing) {
            DWORD process = 0;
            GetWindowThreadProcessId(existing, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(existing, showSettingsMessage, 0, 0);
        }
        return 0;
    }

    const auto path = configFilePath();
    std::string notice;
    Config config = loadConfig(path, notice);
    if (!notice.empty()) notice = "Could not load configuration; using defaults: " + notice;
    Config saved = config;
    Controller controller;
    ControllerWindow owner(instance, controller);
    Frontend frontend(instance);
    TrayManager tray(owner.get());
    frontend.setTray(tray);
    controller.frontend = &frontend;
    controller.tray = &tray;
    Snapshot snapshot;
    tray.update(config, snapshot);
    if (!background && !config.startHidden) frontend.show();

    auto nextPoll = Clock::now();
    auto lastFrame = Clock::now() - std::chrono::seconds(1);
    std::future<Snapshot> polling;
    bool finished = false;
    while (!finished) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { finished = true; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (finished) break;
        auto now = Clock::now();
        if (frontend.closing()) frontend.shutdownRenderer();

        if (polling.valid() && polling.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            snapshot = polling.get();
            tray.recordSample(config, snapshot);
            tray.update(config, snapshot);
        }
        if (!polling.valid() && now >= nextPoll) {
            polling = std::async(std::launch::async, [] { return readAfterburner(); });
            nextPoll = now + std::chrono::milliseconds(config.intervalMs);
            tray.update(config, snapshot);
        }
        if (controller.refreshRequested) {
            tray.update(config, snapshot);
            controller.refreshRequested = false;
        }
        if (controller.exitRequested) {
            bool exit = true;
            if (config != saved) {
                const int answer = MessageBoxW(frontend.visible() ? frontend.window() : owner.get(),
                    L"Save your configuration changes before exiting?", L"RTSSTray", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (answer == IDCANCEL) exit = false;
                if (answer == IDYES) {
                    std::string error;
                    exit = saveConfig(path, config, error);
                    if (!exit) MessageBoxW(owner.get(), utf8ToWide(error).c_str(), L"Could not save", MB_OK | MB_ICONERROR);
                }
            }
            controller.exitRequested = false;
            if (exit) break;
        }

        if (frontend.visible() && now - lastFrame >= std::chrono::milliseconds(33)) {
            const Config previous = config;
            const auto actions = frontend.render(config, saved, snapshot, notice, path, tray.error());
            normalizeConfig(config);
            if (actions.defaults) config = defaultConfig();
            if (actions.revert) { config = saved; notice = "Restored saved configuration"; }
            if (actions.save) {
                std::string error;
                if (saveConfig(path, config, error)) { saved = config; notice = "Configuration saved"; }
                else notice = "Save failed: " + error;
            }
            if (config != previous) {
                tray.update(config, snapshot);
                if (config.intervalMs != previous.intervalMs) nextPoll = Clock::now();
            }
            lastFrame = Clock::now();
        }
        if (frontend.closing()) frontend.shutdownRenderer();
        now = Clock::now();
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(nextPoll - now).count();
        wait = std::clamp<std::int64_t>(wait, 1, 10000);
        if (polling.valid()) wait = std::min<std::int64_t>(wait, 50);
        if (frontend.visible()) {
            const auto frameWait = std::chrono::duration_cast<std::chrono::milliseconds>(lastFrame + std::chrono::milliseconds(33) - now).count();
            wait = std::min(wait, std::max<std::int64_t>(1, frameWait));
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(wait), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    if (polling.valid()) polling.wait();
    controller.frontend = nullptr;
    controller.tray = nullptr;
    return 0;
}

}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try { return rtss::run(instance); }
    catch (const std::exception& ex) {
        MessageBoxW(nullptr, rtss::utf8ToWide(ex.what()).c_str(), L"RTSSTray", MB_OK | MB_ICONERROR);
        return 1;
    }
}
