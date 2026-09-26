// EdgeDock Studio :: app/App.cpp
#include "app/App.h"

#include "core/AppPaths.h"
#include "core/TextConv.h"
#include "modules/SystemUtils.h"

#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace edgedock {
namespace {

constexpr wchar_t kTrayTooltip[] = L"EdgeDock Studio \u00B7 Ctrl+Alt+D";

void FillTrayMenu(HMENU menu, bool focusActive) {
    ::AppendMenuW(menu, MF_STRING, 1, L"Abrir/ocultar panel\tCtrl+Alt+D");
    ::AppendMenuW(menu, MF_STRING | (focusActive ? MF_CHECKED : 0), 2,
                  L"Modo silencioso\tCtrl+Alt+F");
    ::AppendMenuW(menu, MF_STRING, 3, L"Guardar portapapeles como nota\tCtrl+Alt+S");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 4, L"Abrir carpeta de notas");
    ::AppendMenuW(menu, MF_STRING, 5, L"Abrir config.json");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 6, L"Salir de EdgeDock Studio");
}

} // namespace

App::~App() {
    Shutdown();
}

bool App::Initialize(HINSTANCE instance) {
    instance_ = instance;

    // --- Instancia única ---------------------------------------------------------
    instanceMutex_ = ::CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    if (instanceMutex_ != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // Ya hay un EdgeDock: se le pide alternar el panel y este proceso termina.
        HWND existing = ::FindWindowW(ui::EdgeDockWindow::kClassName, nullptr);
        if (existing != nullptr) ::PostMessageW(existing, kMsgWakeUp, 0, 0);
        ::CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        return false;
    }

    config_ = LoadConfig();
    config_.EffectiveNotesFolder();

    // --- Base de datos ------------------------------------------------------------
    const std::wstring databasePath = paths::DatabaseFile();
    if (!database_.Open(databasePath)) {
        paths::AppendLog(L"App: no se pudo abrir la base de datos en " + databasePath);
    }

    if (!CreateSinkWindow()) {
        paths::AppendLog(L"App: no se pudo crear la ventana receptora");
        return false;
    }

    // --- Panel, portapapeles y worker --------------------------------------------
    worker_.Start();
    if (!panel_.Create(instance, database_, clipboard_, config_, worker_)) {
        paths::AppendLog(L"App: no se pudo crear el panel");
        return false;
    }

    modules::ClipboardHook::Settings clipSettings;
    clipSettings.historyLimit = config_.historyLimit;
    clipSettings.maxPreviewChars = config_.maxPreviewChars;
    clipSettings.captureImages = config_.captureImages;
    clipSettings.captureFiles = config_.captureFiles;
    clipSettings.dedupe = config_.dedupeClips;
    if (!clipboard_.Install(panel_.Handle(), &database_, clipSettings)) {
        paths::AppendLog(L"App: el escucha del portapapeles no pudo instalarse");
    }
    clipboard_.SetNotify([this]() { panel_.NotifyNewClip(); });

    AddTrayIcon();
    RegisterHotkeys();
    ::SetTimer(sinkHwnd_, kTimerMaintenance, kMaintenanceIntervalMs, nullptr);
    RunMaintenancePass();
    return true;
}

bool App::CreateSinkWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = SinkProcThunk;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kSinkClassName;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    if (!::RegisterClassExW(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    sinkHwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kSinkClassName,
                                  L"EdgeDock Studio", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  instance_, this);
    return sinkHwnd_ != nullptr;
}

LRESULT CALLBACK App::SinkProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<App*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    auto* self = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) return ::DefWindowProcW(hwnd, message, wParam, lParam);
    return self->SinkProc(message, wParam, lParam);
}

LRESULT App::SinkProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_HOTKEY:
            HandleHotkey(static_cast<int>(wParam));
            return 0;
        case kMsgTrayNotify: {
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    panel_.TogglePanel();
                    return 0;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: {
                    POINT cursor{};
                    ::GetCursorPos(&cursor);
                    ShowTrayMenu(cursor);
                    return 0;
                }
                default:
                    return 0;
            }
        }
        case kMsgWakeUp:
            panel_.TogglePanel();
            return 0;
        case WM_TIMER:
            if (wParam == kTimerMaintenance) RunMaintenancePass();
            return 0;
        case WM_ENDSESSION:
        case WM_QUERYENDSESSION:
            SaveConfig(config_);
            return TRUE;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(sinkHwnd_, message, wParam, lParam);
}

bool App::RegisterHotkeys() {
    unsigned int modifiers = 0;
    unsigned int key = 0;
    bool anyRegistered = false;

    if (ParseHotkey(config_.hotkeyToggle, modifiers, key)) {
        anyRegistered |= ::RegisterHotKey(sinkHwnd_, kHotkeyToggle, modifiers, key) != FALSE;
    }
    if (ParseHotkey(config_.hotkeyFocus, modifiers, key)) {
        anyRegistered |= ::RegisterHotKey(sinkHwnd_, kHotkeyFocus, modifiers, key) != FALSE;
    }
    if (ParseHotkey(config_.hotkeyCaptureNote, modifiers, key)) {
        anyRegistered |= ::RegisterHotKey(sinkHwnd_, kHotkeyCaptureNote, modifiers, key) != FALSE;
    }
    hotkeysRegistered_ = anyRegistered;
    if (!anyRegistered) {
        paths::AppendLog(L"App: ningún atajo global pudo registrarse (¿ya están en uso?)");
    }
    return anyRegistered;
}

void App::UnregisterHotkeys() {
    if (!hotkeysRegistered_) return;
    ::UnregisterHotKey(sinkHwnd_, kHotkeyToggle);
    ::UnregisterHotKey(sinkHwnd_, kHotkeyFocus);
    ::UnregisterHotKey(sinkHwnd_, kHotkeyCaptureNote);
    hotkeysRegistered_ = false;
}

void App::HandleHotkey(int identifier) {
    switch (identifier) {
        case kHotkeyToggle:
            panel_.TogglePanel();
            break;
        case kHotkeyFocus:
            panel_.ToggleFocusMode();
            break;
        case kHotkeyCapture:
            CaptureClipboardAsNote();
            break;
        default:
            break;
    }
}

void App::CaptureClipboardAsNote() {
    std::wstring text;
    if (!modules::ClipboardHook::ReadClipboardText(sinkHwnd_, text) || text::Trim(text).empty()) {
        panel_.SetStatus(L"No hay texto en el portapapeles para guardar", true);
        return;
    }
    const std::wstring folder = config_.EffectiveNotesFolder();
    const std::wstring path = paths::Join(
        folder, text::Format(L"clip-%s.md", paths::TimestampForFile().c_str()));
    const std::string payload = text::ToUtf8(text);
    if (paths::WriteTextFileAtomic(path, payload)) {
        panel_.SetStatus(text::Format(L"Nota guardada: %s", paths::FileNameOf(path).c_str()), false);
        if (config_.topMost) worker_.Post([folder]() {
            // Abrir la carpeta en segundo plano: el panel nunca se bloquea por el shell.
            sysutil::OpenFolder(folder);
        });
    } else {
        panel_.SetStatus(L"No se pudo escribir la nota", true);
    }
}

void App::RunMaintenancePass() {
    const int limit = config_.historyLimit;
    const int keepDays = config_.historyKeepDays;
    data::ClipDatabase* database = &database_;
    const HWND panelHandle = panel_.Handle();

    // Sin capturar punteros a la App: el trabajo solo toca la base y, si podó algo, pide
    // al panel una recarga por mensaje.
    worker_.Post([database, limit, keepDays, panelHandle]() {
        const int removed = database->PruneUnpinned(limit, keepDays, paths::NowUnixSeconds());
        if (removed > 0) {
            paths::AppendLog(text::Format(L"Mantenimiento: %d clips podados por antigüedad/volumen", removed));
            ::PostMessageW(panelHandle, ui::EdgeDockWindow::kMsgRefreshClips, 0, 0);
        }
    });
}

bool App::AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = sinkHwnd_;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kMsgTrayNotify;
    data.hIcon = BuildTrayIcon(::GetSystemMetrics(SM_CXSMICON) > 0 ? ::GetSystemMetrics(SM_CXSMICON) : 16);
    if (data.hIcon == nullptr) data.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::wcsncpy_s(data.szTip, kTrayTooltip, _TRUNCATE);

    if (!::Shell_NotifyIconW(NIM_ADD, &data)) {
        paths::AppendLog(L"App: Shell_NotifyIcon falló");
        if (data.hIcon != nullptr) ::DestroyIcon(data.hIcon);
        return false;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &data);
    trayIconAdded_ = true;
    return true;
}

void App::RemoveTrayIcon() {
    if (!trayIconAdded_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = sinkHwnd_;
    data.uID = 1;
    ::Shell_NotifyIconW(NIM_DELETE, &data);
    trayIconAdded_ = false;
}

void App::ShowTrayMenu(POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return;
    FillTrayMenu(menu, panel_.IsExpanded());

    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    info.hbrBack = ::CreateSolidBrush(RGB(4, 6, 8));
    ::SetMenuInfo(menu, &info);

    ::SetForegroundWindow(sinkHwnd_);
    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x,
                                          screenPoint.y, 0, sinkHwnd_, nullptr);
    ::DestroyMenu(menu);
    if (info.hbrBack != nullptr) ::DeleteObject(info.hbrBack);

    switch (command) {
        case 1:
            panel_.TogglePanel();
            break;
        case 2:
            panel_.ToggleFocusMode();
            break;
        case 3:
            CaptureClipboardAsNote();
            break;
        case 4:
            sysutil::OpenFolder(config_.EffectiveNotesFolder());
            break;
        case 5:
            sysutil::EditTextFile(paths::ConfigFile());
            break;
        case 6:
            ::PostQuitMessage(0);
            break;
        default:
            break;
    }
}

HICON App::BuildTrayIcon(int size) const {
    if (size < 8) size = 16;
    const int width = size;
    const int height = size;

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;   // negativo: fila superior primero
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP color = ::CreateDIBSection(memory, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
                                       &bits, nullptr, 0);
    if (color == nullptr || bits == nullptr) {
        ::DeleteDC(memory);
        ::ReleaseDC(nullptr, screen);
        return nullptr;
    }

    // Dibujo del icono: barra cian en el borde derecho, marco púrpura y fondo transparente.
    auto* pixels = static_cast<uint32_t*>(bits);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width - 1);
            const float fy = static_cast<float>(y) / static_cast<float>(height - 1);
            uint32_t pixel = 0x00000000;   // ARGB transparente

            const bool frame = fx > 0.14f && fx < 0.92f && fy > 0.14f && fy < 0.92f;
            const bool rightBar = fx > 0.78f;
            const bool bottomBar = fy > 0.78f;

            if (rightBar && frame) {
                pixel = 0xFF00FFFF;   // cian neón puro (0,255,255)
            } else if (bottomBar && frame) {
                pixel = 0xFFA020F0;   // púrpura (160,32,240)
            } else if (frame) {
                pixel = 0xFF101418;   // carbono casi negro
            }
            pixels[y * width + x] = pixel;
        }
    }
    ::SelectObject(memory, color);

    HBITMAP mask = ::CreateBitmap(width, height, 1, 1, nullptr);
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = mask;
    HICON icon = ::CreateIconIndirect(&iconInfo);

    ::DeleteObject(mask);
    ::DeleteObject(color);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return icon;
}

int App::Run() {
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::Shutdown() {
    if (sinkHwnd_ != nullptr) {
        ::KillTimer(sinkHwnd_, kTimerMaintenance);
        UnregisterHotkeys();
        RemoveTrayIcon();
    }
    clipboard_.Uninstall();

    // El worker termina primero: drena lo pendiente antes de cerrar la base de datos.
    worker_.Stop();
    panel_.Destroy();

    if (sinkHwnd_ != nullptr) {
        ::DestroyWindow(sinkHwnd_);
        sinkHwnd_ = nullptr;
    }
    ::UnregisterClassW(kSinkClassName, instance_);

    if (database_.IsOpen()) {
        database_.PruneUnpinned(config_.historyLimit, config_.historyKeepDays, paths::NowUnixSeconds());
        database_.Close();
    }
    SaveConfig(config_);

    if (instanceMutex_ != nullptr) {
        ::CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
}

} // namespace edgedock
