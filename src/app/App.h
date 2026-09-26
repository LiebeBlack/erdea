#pragma once
// EdgeDock Studio :: app/App.h
// Propietario de los subsistemas: base de datos, escucha de portapapeles, worker asíncrono,
// ventana del panel, icono de bandeja y atajos globales.

#include <windows.h>

#include <string>

#include "core/AsyncWorker.h"
#include "core/Config.h"
#include "data/ClipDatabase.h"
#include "modules/ClipboardHook.h"
#include "ui/EdgeDockWindow.h"

namespace edgedock {

class App {
public:
    App() = default;
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool Initialize(HINSTANCE instance);
    int Run();
    void Shutdown();

    // Notificaciones de sistema que el panel reenvía al hilo de UI.
    static constexpr UINT kMsgTrayNotify = WM_APP + 0x70;
    static constexpr UINT kMsgWakeUp = WM_APP + 0x71;

private:
    static LRESULT CALLBACK SinkProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT SinkProc(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateSinkWindow();
    bool RegisterHotkeys();
    void UnregisterHotkeys();
    bool AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    void HandleHotkey(int identifier);
    void RunMaintenancePass();
    void CaptureClipboardAsNote();
    HICON BuildTrayIcon(int size) const;

    HINSTANCE instance_ = nullptr;
    HWND sinkHwnd_ = nullptr;
    HANDLE instanceMutex_ = nullptr;
    bool trayIconAdded_ = false;
    bool hotkeysRegistered_ = false;

    Config config_{};
    data::ClipDatabase database_{};
    modules::ClipboardHook clipboard_{};
    AsyncWorker worker_{};
    ui::EdgeDockWindow panel_{};

    static constexpr const wchar_t* kSinkClassName = L"EdgeDockStudio.TraySink";
    static constexpr const wchar_t* kInstanceMutexName = L"Local\\EdgeDockStudio.Singleton";
    static constexpr UINT_PTR kTimerMaintenance = 1;
    static constexpr UINT kMaintenanceIntervalMs = 5 * 60 * 1000;
    static constexpr int kHotkeyToggle = 1;
    static constexpr int kHotkeyFocus = 2;
    static constexpr int kHotkeyCapture = 3;
};

} // namespace edgedock
