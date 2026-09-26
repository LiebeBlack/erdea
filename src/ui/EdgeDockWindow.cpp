// EdgeDock Studio :: ui/EdgeDockWindow.cpp
#include "ui/EdgeDockWindow.h"

#include "core/AppPaths.h"
#include "core/Json.h"
#include "core/TextConv.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "comctl32.lib")

namespace edgedock::ui {
namespace {

INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitCommonControlsOnce(PINIT_ONCE, PVOID, PVOID*) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&controls);
    return TRUE;
}

constexpr int kTabCount = 4;
constexpr int kIdSearchEdit = 40001;
constexpr int kIdMenuBase = 41000;

const wchar_t* TabLabel(PanelTab tab) {
    switch (tab) {
        case PanelTab::Clipboard: return L"CLIPS";
        case PanelTab::Text: return L"TEXTO";
        case PanelTab::Notes: return L"NOTAS";
        case PanelTab::System: return L"SISTEMA";
    }
    return L"";
}

// --- Callbacks OLE sin std::function (contexto = ScratchpadManager) ------------
void NoteDropText(void* context, const wchar_t* text, size_t length, DWORD effect) {
    (void)effect;
    auto* manager = static_cast<modules::ScratchpadManager*>(context);
    if (manager != nullptr) manager->CreateNoteFromText(text, length, nullptr);
}

void NoteDropFiles(void* context, const wchar_t* const* paths, size_t count, DWORD effect) {
    (void)effect;
    auto* manager = static_cast<modules::ScratchpadManager*>(context);
    if (manager == nullptr) return;
    for (size_t index = 0; index < count; ++index) {
        if (paths[index] == nullptr) continue;
        const std::wstring path(paths[index]);
        const std::wstring extension = paths::ExtensionOf(path);
        if (extension == L".md" || extension == L".txt" || extension == L".markdown" || extension.empty()) {
            if (manager->CreateNoteFromFile(path, nullptr)) continue;
        }
        // Cualquier otro archivo se archiva como nota con su ruta y su contenido textual.
        const std::wstring noteLine = edgedock::text::Format(L"Archivo soltado: %s", path.c_str());
        manager->CreateNoteFromText(noteLine.c_str(), noteLine.size(), nullptr);
    }
}

void NoteDragState(void* context, bool active, DWORD effect) {
    auto* manager = static_cast<modules::ScratchpadManager*>(context);
    if (manager != nullptr) manager->NotifyDropActive(active, effect);
}

modules::DropCallbacks MakeNoteDropCallbacks(modules::ScratchpadManager* manager) {
    modules::DropCallbacks callbacks;
    callbacks.onText = &NoteDropText;
    callbacks.onFiles = &NoteDropFiles;
    callbacks.onDragState = &NoteDragState;
    callbacks.context = manager;
    return callbacks;
}

} // namespace

EdgeDockWindow::~EdgeDockWindow() {
    Destroy();
}

// -----------------------------------------------------------------------------
// Ciclo de vida
// -----------------------------------------------------------------------------

bool EdgeDockWindow::Create(HINSTANCE instance, data::ClipDatabase& database,
                            modules::ClipboardHook& clips, Config& config, AsyncWorker& worker) {
    instance_ = instance;
    database_ = &database;
    clips_ = &clips;
    config_ = &config;
    worker_ = &worker;
    palette_ = theme::PaletteFor(config.theme);

    ::InitOnceExecuteOnce(&g_initOnce, InitCommonControlsOnce, nullptr, nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = WndProcThunk;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kClassName;
    windowClass.lpszMenuName = nullptr;
    windowClass.hIcon = nullptr;
    if (!::RegisterClassExW(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        paths::AppendLog(L"EdgeDockWindow: RegisterClassExW falló");
        return false;
    }

    UpdateMonitorBounds();
    dpi_ = static_cast<int>(::GetDpiForSystem());

    windowHeight_ = workArea_.bottom - workArea_.top;
    anchorY_ = workArea_.top;
    const int collapsedPx = static_cast<int>(std::lround(config.collapsedDip * DipToPx(1.0f)));

    DWORD extendedStyle = WS_EX_TOOLWINDOW;
    if (config.topMost) extendedStyle |= WS_EX_TOPMOST;

    hwnd_ = ::CreateWindowExW(extendedStyle, kClassName, L"EdgeDock Studio", WS_POPUP,
                              workArea_.right - collapsedPx, anchorY_, collapsedPx, windowHeight_,
                              nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        paths::AppendLog(L"EdgeDockWindow: CreateWindowExW falló");
        return false;
    }

    expandProgress_ = config.startExpanded ? 1.0f : 0.0f;
    targetExpanded_ = config.startExpanded;
    ApplyWindowRect(config.collapsedDip +
                    (config.panelWidthDip - config.collapsedDip) * expandProgress_);

    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ::SetTimer(hwnd_, kTimerEdge, kEdgePollMs, nullptr);
    ::SetTimer(hwnd_, kTimerStatus, 1000, nullptr);
    ::SetTimer(hwnd_, kTimerStats, 1000, nullptr);

    pythonInterpreter_ = sysutil::ResolvePythonInterpreter();
    captureSummary_ = L"esperando copias";

    // Notas rápidas + destino OLE: el panel entero acepta texto y archivos soltados.
    notes_.Initialize(hwnd_, config);
    dropTarget_ = new modules::PanelDropTarget(MakeNoteDropCallbacks(&notes_));
    if (modules::RegisterPanelDropTarget(hwnd_, dropTarget_) != S_OK) {
        paths::AppendLog(L"EdgeDockWindow: RegisterDragDrop falló; el panel no aceptará soltados");
    }

    SetStatus(L"EdgeDock Studio listo \u00B7 Ctrl+Alt+D para alternar", false);
    UpdateTextPreview();
    UpdateSystemSnapshot(true);
    return true;
}

void EdgeDockWindow::Destroy() {
    if (dropTarget_ != nullptr) {
        modules::RevokePanelDropTarget(hwnd_);
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
    notes_.Shutdown();

    if (hwnd_ != nullptr) {
        ::KillTimer(hwnd_, kTimerAnimation);
        ::KillTimer(hwnd_, kTimerEdge);
        ::KillTimer(hwnd_, kTimerStatus);
        ::KillTimer(hwnd_, kTimerStats);
        if (searchEdit_ != nullptr) {
            ::DestroyWindow(searchEdit_);
            searchEdit_ = nullptr;
        }
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    renderer_.Destroy();
    ::UnregisterClassW(kClassName, instance_);
    ReleaseSuspendedProcesses();
}

LRESULT CALLBACK EdgeDockWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<EdgeDockWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) self->hwnd_ = hwnd;
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    auto* self = reinterpret_cast<EdgeDockWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) return ::DefWindowProcW(hwnd, message, wParam, lParam);

    // WM_CLIPBOARDUPDATE y WM_HOTKEY llegan al hilo; el panel los resuelve aquí mismo.
    if (message == WM_CLIPBOARDUPDATE) {
        if (self->clips_ != nullptr && self->clips_->HandleClipboardUpdate()) self->NotifyNewClip();
        return 0;
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT EdgeDockWindow::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            OnCreate();
            return 0;
        case WM_ERASEBKGND:
            return 1;   // Direct2D repinta todo: se evita el parpadeo del borrado GDI
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_SIZE:
            OnSize(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_TIMER:
            OnTimer(static_cast<UINT_PTR>(wParam));
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE:
            OnMouseLeave();
            return 0;
        case WM_MOUSEWHEEL:
            OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_LBUTTONDOWN:
            OnLeftButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            OnLeftButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONDBLCLK:
            OnLeftButtonDoubleClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_RBUTTONUP:
            OnRightButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_KEYDOWN:
            OnKeyDown(wParam);
            return 0;
        case WM_COMMAND:
            OnCommand(wParam, lParam);
            return 0;
        case modules::ScratchpadManager::kMsgNoteSaved:
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case modules::ScratchpadManager::kMsgNoteListChanged:
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetTextColor(dc, RGB(236, 242, 246));
            ::SetBkColor(dc, RGB(0, 0, 0));
            static HBRUSH brush = ::CreateSolidBrush(RGB(0, 0, 0));
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_DPICHANGED:
            dpi_ = HIWORD(wParam);
            OnDisplayChange();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            OnDisplayChange();
            return 0;
        case WM_CLOSE:
            // Cerrar el panel no mata la aplicación: se repliega contra el borde.
            Collapse();
            return 0;
        case WM_DESTROY:
            OnDestroy();
            return 0;
        case kWmEdgeDockTick:
            PollEdgeTrigger();
            return 0;
        case EdgeDockWindow::kMsgRefreshClips:
            refreshPending_ = false;
            RefreshClips();
            return 0;
        case kMsgWorkerCallback:
            AsyncWorker::DispatchMainCallback(lParam);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

void EdgeDockWindow::OnCreate() {
    renderer_.SetPalette(palette_);
    renderer_.Create(hwnd_);
    dpi_ = static_cast<int>(::GetDpiForWindow(hwnd_));
    CreateSearchControl();
    UpdateMonitorBounds();
    ApplyWindowRect(config_->collapsedDip +
                    (config_->panelWidthDip - config_->collapsedDip) * expandProgress_);
}

void EdgeDockWindow::OnDestroy() {
    if (clips_ != nullptr) clips_->Uninstall();
}

// -----------------------------------------------------------------------------
// Geometría, borde y animación
// -----------------------------------------------------------------------------

float EdgeDockWindow::DipToPx(float value) const {
    return value * (static_cast<float>(dpi_) / 96.0f);
}

float EdgeDockWindow::PxToDip(int value) const {
    return static_cast<float>(value) * 96.0f / static_cast<float>(dpi_ == 0 ? 96 : dpi_);
}

void EdgeDockWindow::UpdateMonitorBounds() {
    POINT cursor{};
    ::GetCursorPos(&cursor);
    HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) {
        monitorRect_ = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
        workArea_ = monitorRect_;
        return;
    }
    monitorRect_ = info.rcMonitor;
    workArea_ = info.rcWork;
    // La barra de tareas puede estar arriba o abajo: el panel ocupa el área útil completa.
    if (workArea_.right - workArea_.left < 240) workArea_ = monitorRect_;
}

void EdgeDockWindow::ApplyWindowRect(float visibleDip) {
    if (hwnd_ == nullptr) return;
    int widthPx = static_cast<int>(std::lround(DipToPx(visibleDip)));
    if (widthPx < 2) widthPx = 2;
    if (widthPx > workArea_.right - workArea_.left) widthPx = workArea_.right - workArea_.left;
    windowWidthPx_ = widthPx;
    anchorX_ = workArea_.right - widthPx;
    anchorY_ = workArea_.top;
    windowHeight_ = workArea_.bottom - workArea_.top;

    ::SetWindowPos(hwnd_, config_->topMost ? HWND_TOPMOST : HWND_TOP, anchorX_, anchorY_, widthPx,
                   windowHeight_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::Expand() {
    if (hwnd_ == nullptr || targetExpanded_) return;
    targetExpanded_ = true;
    collapseDeadline_ = 0;
    StartAnimation();
    ::SetForegroundWindow(hwnd_);
}

void EdgeDockWindow::Collapse() {
    if (hwnd_ == nullptr || !targetExpanded_) return;
    targetExpanded_ = false;
    collapseDeadline_ = 0;
    pointerInside_ = false;
    if (searchEdit_ != nullptr) {
        ::SetWindowTextW(searchEdit_, L"");
        CommitSearch();
        ::ShowWindow(searchEdit_, SW_HIDE);
    }
    // El editor de notas se oculta con el panel, pero su contenido ya está a salvo.
    notes_.FlushNow();
    notes_.ShowEditor(false);
    if (::GetForegroundWindow() == hwnd_) ::SetForegroundWindow(::GetShellWindow());
    StartAnimation();
}

void EdgeDockWindow::TogglePanel() {
    if (targetExpanded_) {
        Collapse();
    } else {
        Expand();
    }
}

void EdgeDockWindow::StartAnimation() {
    animationRunning_ = true;
    animationStartTick_ = ::GetTickCount64();
    animationStartProgress_ = expandProgress_;
    ::SetTimer(hwnd_, kTimerAnimation, kAnimationFrameMs, nullptr);
}

void EdgeDockWindow::TickAnimation() {
    const float duration = static_cast<float>(std::max(40, config_->animationMs));
    const float elapsed = static_cast<float>(::GetTickCount64() - animationStartTick_);
    float t = elapsed / duration;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;

    // Suavizado tipo smoothstep: arranque y frenada progresivos, sin rebote.
    const float eased = t * t * (3.0f - 2.0f * t);
    const float goal = targetExpanded_ ? 1.0f : 0.0f;
    expandProgress_ = animationStartProgress_ + (goal - animationStartProgress_) * eased;

    if (t >= 1.0f) {
        expandProgress_ = goal;
        animationRunning_ = false;
        ::KillTimer(hwnd_, kTimerAnimation);
        if (expandProgress_ > 0.5f) {
            LayoutSearchControl();
            if (searchEdit_ != nullptr) ::ShowWindow(searchEdit_, SW_SHOW);
        }
    }

    ApplyWindowRect(config_->collapsedDip +
                    (config_->panelWidthDip - config_->collapsedDip) * expandProgress_);
}

void EdgeDockWindow::PollEdgeTrigger() {
    if (hwnd_ == nullptr) return;

    POINT cursor{};
    if (!::GetCursorPos(&cursor)) return;
    cursorPx_ = cursor;

    HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) && (info.rcWork.right != workArea_.right ||
                                             info.rcWork.top != workArea_.top ||
                                             info.rcWork.bottom != workArea_.bottom)) {
        UpdateMonitorBounds();
        ApplyWindowRect(config_->collapsedDip +
                        (config_->panelWidthDip - config_->collapsedDip) * expandProgress_);
    }

    const int edgeX = workArea_.right - 1;
    const bool atEdge = cursor.x >= edgeX && cursor.y >= workArea_.top && cursor.y < workArea_.bottom;

    RECT windowRect{};
    ::GetWindowRect(hwnd_, &windowRect);
    const bool insideWindow = cursor.x >= windowRect.left && cursor.x < windowRect.right + 1 &&
                              cursor.y >= windowRect.top && cursor.y < windowRect.bottom;
    const bool foreground = ::GetForegroundWindow() == hwnd_;
    const bool busyWithSearch =
        searchEdit_ != nullptr && ::GetFocus() == searchEdit_ && !::GetWindowTextLengthW(searchEdit_) == false;

    if (!targetExpanded_) {
        if (atEdge) {
            Expand();
        }
        return;
    }

    if (insideWindow) {
        collapseDeadline_ = 0;
        return;
    }
    if (foreground || busyWithSearch) {
        // Con el panel en primer plano o escribiendo en la búsqueda nunca se repliega solo.
        collapseDeadline_ = 0;
        return;
    }
    if (!config_->autoCollapseOnBlur) return;

    const ULONGLONG now = ::GetTickCount64();
    if (collapseDeadline_ == 0) {
        collapseDeadline_ = now + static_cast<ULONGLONG>(std::max(100, config_->collapseDelayMs));
        return;
    }
    if (now >= collapseDeadline_) Collapse();
}

void EdgeDockWindow::OnTimer(UINT_PTR timerId) {
    switch (timerId) {
        case kTimerAnimation:
            TickAnimation();
            break;
        case kTimerEdge:
            PollEdgeTrigger();
            break;
        case kTimerStatus:
            if (statusUntil_ != 0 && ::GetTickCount64() >= statusUntil_) {
                statusText_.clear();
                statusUntil_ = 0;
                ::InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        case kTimerStats:
            UpdateSystemSnapshot(false);
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        default:
            break;
    }
}

void EdgeDockWindow::OnSize(int width, int height) {
    renderer_.Resize(static_cast<UINT>(std::max(0, width)), static_cast<UINT>(std::max(0, height)));
    LayoutSearchControl();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::OnDisplayChange() {
    UpdateMonitorBounds();
    const float visible = config_->collapsedDip +
                          (config_->panelWidthDip - config_->collapsedDip) * expandProgress_;
    ApplyWindowRect(visible);
    LayoutSearchControl();
    UpdateTextPreview();
    UpdateSystemSnapshot(true);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::SetStatus(const std::wstring& text, bool warn) {
    statusText_ = text;
    statusWarn_ = warn;
    statusUntil_ = ::GetTickCount64() + 4200;
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::RefreshClips() {
    if (clips_ == nullptr) return;
    clips_->Refresh();
    if (selectedClip_ >= static_cast<int>(clips_->VisibleCount())) selectedClip_ = -1;
    scrollTarget_ = 0.0f;
    UpdateTextPreview();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::NotifyNewClip() {
    if (clips_ == nullptr) return;
    captureSummary_ = clips_->LastCaptureSummary();
    // Antirrebote: una ráfaga de copias produce una sola recarga del historial.
    if (refreshPending_) return;
    refreshPending_ = true;
    ::PostMessageW(hwnd_, kMsgRefreshClips, 0, 0);
}

void EdgeDockWindow::ToggleTab(PanelTab tab) {
    tab_ = tab;
    scrollTarget_ = scrollOffset_ = 0.0f;
    if (tab_ == PanelTab::Notes) {
        // El EDIT de notas se crea la primera vez que se entra en la pestaña.
        notes_.EnsureEditor();
        notesList_.target = 0.0f;
    }
    LayoutSearchControl();
    if (tab_ == PanelTab::Text) UpdateTextPreview();
    if (tab_ == PanelTab::System) UpdateSystemSnapshot(true);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::FocusSearch() {
    if (searchEdit_ == nullptr) return;
    Expand();
    ::SetFocus(searchEdit_);
    ::SendMessageW(searchEdit_, EM_SETSEL, 0, -1);
}

// -----------------------------------------------------------------------------
// Entrada
// -----------------------------------------------------------------------------

void EdgeDockWindow::UpdateInputState() {
    const float scale = DipToPx(1.0f);
    POINT cursor{};
    ::GetCursorPos(&cursor);
    POINT client = cursor;
    ::ScreenToClient(hwnd_, &client);

    input_.x = PxToDip(client.x);
    input_.y = PxToDip(client.y);
    input_.inside = (client.x >= 0 && client.y >= 0 && client.x < windowWidthPx_ &&
                     client.y < windowHeight_);
    input_.down = (::GetKeyState(VK_LBUTTON) & 0x8000) != 0;
    input_.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    input_.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    input_.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    (void)scale;
}

void EdgeDockWindow::OnMouseMove(int x, int y) {
    (void)x;
    (void)y;
    pointerInside_ = true;
    if (!trackingLeave_) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        if (::TrackMouseEvent(&track)) trackingLeave_ = true;
    }
    UpdateInputState();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::OnMouseLeave() {
    trackingLeave_ = false;
    pointerInside_ = false;
    input_.inside = false;
    input_.x = -1.0f;
    input_.y = -1.0f;
    hoveredClip_ = -1;
    hoveredTab_ = -1;
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::OnMouseWheel(int delta) {
    if (!targetExpanded_) return;
    input_.wheelDelta += delta;
    UpdateInputState();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::OnLeftButtonDown(int x, int y) {
    (void)x;
    (void)y;
    UpdateInputState();
    input_.pressed = true;
    input_.down = true;
    ::SetCapture(hwnd_);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::OnLeftButtonUp(int x, int y) {
    (void)x;
    (void)y;
    UpdateInputState();
    input_.released = true;
    input_.down = false;
    if (::GetCapture() == hwnd_) ::ReleaseCapture();
    draggingScrollbar_ = false;
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::OnLeftButtonDoubleClick(int x, int y) {
    (void)x;
    (void)y;
    UpdateInputState();
    input_.doubleClick = true;
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::OnRightButtonUp(int x, int y) {
    if (!targetExpanded_) return;
    UpdateInputState();
    POINT screen{x, y};
    ::ClientToScreen(hwnd_, &screen);

    if (tab_ == PanelTab::Clipboard && hoveredClip_ >= 0) {
        selectedClip_ = hoveredClip_;
        ShowClipContextMenu(hoveredClip_, screen);
        return;
    }
    ShowClipContextMenu(-1, screen);
}

void EdgeDockWindow::OnKeyDown(WPARAM key) {
    const int visibleCount = clips_ != nullptr ? static_cast<int>(clips_->VisibleCount()) : 0;
    switch (key) {
        case VK_ESCAPE:
            if (searchEdit_ != nullptr && ::GetWindowTextLengthW(searchEdit_) > 0) {
                ClearSearch();
            } else {
                Collapse();
            }
            break;
        case VK_DOWN:
            if (visibleCount > 0) {
                SelectClip(std::min(selectedClip_ + 1, visibleCount - 1));
            }
            break;
        case VK_UP:
            if (visibleCount > 0) {
                SelectClip(selectedClip_ <= 0 ? 0 : selectedClip_ - 1);
            }
            break;
        case VK_NEXT:
            if (visibleCount > 0) SelectClip(std::min(selectedClip_ + 8, visibleCount - 1));
            break;
        case VK_PRIOR:
            if (visibleCount > 0) SelectClip(std::max(selectedClip_ - 8, 0));
            break;
        case VK_RETURN:
            if (selectedClip_ >= 0) CopyVisibleClip(selectedClip_);
            break;
        case VK_DELETE:
            if (selectedClip_ >= 0 && clips_ != nullptr) {
                const int index = selectedClip_;
                if (clips_->DeleteAt(static_cast<size_t>(index))) {
                    SetStatus(L"Clip eliminado del historial", true);
                    RefreshClips();
                }
            }
            break;
        case 'P':
            if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0 && selectedClip_ >= 0) {
                RunClipCommand(kIdMenuBase + 1, selectedClip_);
            }
            break;
        case 'F':
            if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0) FocusSearch();
            break;
        case 'C':
            if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0 && selectedClip_ >= 0) CopyVisibleClip(selectedClip_);
            break;
        default:
            break;
    }
}

void EdgeDockWindow::OnCommand(WPARAM controlParam, LPARAM controlHandle) {
    const int controlId = LOWORD(controlParam);
    const int notification = HIWORD(controlParam);
    HWND control = reinterpret_cast<HWND>(controlHandle);

    // Cada pulsación en el EDIT de notas dispara el autoguardado diferido.
    if (control != nullptr && control == notes_.EditorHandle()) {
        if (notification == EN_CHANGE) notes_.MarkDirty();
        return;
    }
    if (controlId == kIdSearchEdit) CommitSearch();
}

void EdgeDockWindow::OnPaint() {
    PAINTSTRUCT paint{};
    ::BeginPaint(hwnd_, &paint);
    Paint();
    ::EndPaint(hwnd_, &paint);
}

// -----------------------------------------------------------------------------
// Diseño, control de búsqueda e interacción
// -----------------------------------------------------------------------------

void EdgeDockWindow::ComputeLayout() {
    const float visibleDip = config_->collapsedDip +
                             (config_->panelWidthDip - config_->collapsedDip) * expandProgress_;
    // El contenido se ancla al borde derecho: durante el despliegue entra deslizando.
    contentOriginX_ = visibleDip - config_->panelWidthDip;

    const float pad = theme::kPad;
    const float left = contentOriginX_ + pad;
    const float right = contentOriginX_ + config_->panelWidthDip - pad;

    float y = 20.0f;
    headerRect_ = Rectf{left, y, right, y + 40.0f};
    y = headerRect_.bottom + 12.0f;
    tabsRect_ = Rectf{left, y, right, y + 30.0f};
    y = tabsRect_.bottom + 12.0f;
    searchRect_ = Rectf{left, y, right, y + 36.0f};
    y = searchRect_.bottom + 12.0f;

    const float statusHeight = 34.0f;
    statusRect_ = Rectf{left, static_cast<float>(windowHeightDip_) - statusHeight - 10.0f, right,
                        static_cast<float>(windowHeightDip_) - 10.0f};
    contentRect_ = Rectf{contentOriginX_ + 6.0f, y, contentOriginX_ + config_->panelWidthDip - 6.0f,
                         statusRect_.top - 10.0f};
    contentWidthDip_ = config_->panelWidthDip - 12.0f;

    // La lista de notas ocupa la franja superior del área de contenido; el EDIT, el resto.
    notesListBottomDip_ = std::min(contentRect_.top + 206.0f, contentRect_.bottom - 120.0f);

    const float rowStride = theme::kRowHeight + theme::kRowGap;
    const size_t count = clips_ != nullptr ? clips_->VisibleCount() : 0;
    const float total = static_cast<float>(count) * rowStride;
    scrollMaximum_ = std::max(0.0f, total - contentRect_.Height());
    ClampScroll();
}

void EdgeDockWindow::CreateSearchControl() {
    if (searchEdit_ != nullptr) return;
    searchEdit_ = ::CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                                    0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSearchEdit)),
                                    instance_, nullptr);
    if (searchEdit_ == nullptr) {
        paths::AppendLog(L"EdgeDockWindow: no se pudo crear el control de búsqueda");
        return;
    }

    // Marca de agua del control nativo mientras no hay consulta.
    ::SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Buscar en el historial\u2026  (Ctrl+F)"));
    ::SendMessageW(searchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                   MAKELPARAM(8, 8));
    ::SendMessageW(searchEdit_, EM_SETLIMITTEXT, 256, 0);
    ::SendMessageW(searchEdit_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void EdgeDockWindow::LayoutSearchControl() {
    if (searchEdit_ == nullptr) return;
    // El EDIT del buscador solo existe en la pestaña de clips; el de notas, en la suya.
    const bool searchVisible = expandProgress_ >= 0.75f && tab_ == PanelTab::Clipboard;
    ::ShowWindow(searchEdit_, searchVisible ? SW_SHOW : SW_HIDE);
    if (!searchVisible) {
        LayoutNotesEditor();
        return;
    }
    LayoutNotesEditor();

    const float scale = DipToPx(1.0f);
    const int x = static_cast<int>(std::lround((searchRect_.left + 8.0f) * scale));
    const int y = static_cast<int>(std::lround((searchRect_.top + 7.0f) * scale));
    const int width = static_cast<int>(std::lround((searchRect_.Width() - 16.0f) * scale));
    const int height = static_cast<int>(std::lround((searchRect_.Height() - 14.0f) * scale));
    ::SetWindowPos(searchEdit_, nullptr, std::max(0, x), std::max(0, y), std::max(10, width),
                   std::max(10, height), SWP_NOZORDER | SWP_NOACTIVATE);
    ::ShowWindow(searchEdit_, SW_SHOW);

    // Fuente proporcional acorde al DPI del monitor.
    const int fontHeight = -MulDiv(11, dpi_, 72);
    HFONT font = ::CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, theme::kFontUi);
    if (font != nullptr) {
        ::SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (searchFont_ != nullptr) ::DeleteObject(searchFont_);
        searchFont_ = font;
    }
}

void EdgeDockWindow::CommitSearch() {
    if (clips_ == nullptr || searchEdit_ == nullptr) return;
    const int length = ::GetWindowTextLengthW(searchEdit_);
    std::wstring query(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) ::GetWindowTextW(searchEdit_, query.data(), length + 1);
    query.resize(static_cast<size_t>(length));
    clips_->SetQuery(query);
    selectedClip_ = clips_->VisibleCount() > 0 ? 0 : -1;
    scrollTarget_ = scrollOffset_ = 0.0f;
    UpdateTextPreview();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::ClearSearch() {
    if (searchEdit_ != nullptr) ::SetWindowTextW(searchEdit_, L"");
    CommitSearch();
}

int EdgeDockWindow::HitTestClipRow(float dipY) const {
    if (clips_ == nullptr || tab_ != PanelTab::Clipboard) return -1;
    if (dipY < contentRect_.top || dipY >= contentRect_.bottom) return -1;
    const float rowStride = theme::kRowHeight + theme::kRowGap;
    const float local = dipY - contentRect_.top + scrollOffset_;
    if (local < 0.0f) return -1;
    const int index = static_cast<int>(local / rowStride);
    if (index < 0 || static_cast<size_t>(index) >= clips_->VisibleCount()) return -1;
    return index;
}

float EdgeDockWindow::RowTop(size_t visibleIndex) const {
    return contentRect_.top - scrollOffset_ +
           static_cast<float>(visibleIndex) * (theme::kRowHeight + theme::kRowGap);
}

void EdgeDockWindow::ClampScroll() {
    if (scrollTarget_ < 0.0f) scrollTarget_ = 0.0f;
    if (scrollTarget_ > scrollMaximum_) scrollTarget_ = scrollMaximum_;
    if (scrollOffset_ < 0.0f) scrollOffset_ = 0.0f;
    if (scrollOffset_ > scrollMaximum_) scrollOffset_ = scrollMaximum_;
}

void EdgeDockWindow::EnsureVisible(int visibleIndex) {
    if (visibleIndex < 0) return;
    const float rowStride = theme::kRowHeight + theme::kRowGap;
    const float top = static_cast<float>(visibleIndex) * rowStride;
    const float bottom = top + theme::kRowHeight;
    if (top < scrollTarget_) scrollTarget_ = top;
    if (bottom > scrollTarget_ + contentRect_.Height()) {
        scrollTarget_ = bottom - contentRect_.Height();
    }
    ClampScroll();
}

void EdgeDockWindow::SelectClip(int visibleIndex, bool focusSearch) {
    if (clips_ == nullptr) return;
    if (visibleIndex < 0 || static_cast<size_t>(visibleIndex) >= clips_->VisibleCount()) return;
    selectedClip_ = visibleIndex;
    EnsureVisible(visibleIndex);
    UpdateTextPreview();
    if (focusSearch) FocusSearch();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::CopyVisibleClip(int visibleIndex) {
    if (clips_ == nullptr) return;
    const data::ClipEntry* entry = clips_->Visible(static_cast<size_t>(visibleIndex));
    if (entry == nullptr) return;
    if (clips_->CopyToClipboard(static_cast<size_t>(visibleIndex))) {
        const sysutil::TextStats stats = sysutil::AnalyzeText(entry->text);
        SetStatus(text::Format(L"Copiado · %s", stats.Compact().c_str()), false);
    } else {
        SetStatus(L"El portapapeles está bloqueado por otra aplicación", true);
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::ResetTransientInput() {
    input_.pressed = false;
    input_.released = false;
    input_.doubleClick = false;
    input_.wheelDelta = 0;
}

theme::Rgba EdgeDockWindow::Accent(float highlight) const {
    return theme::Mix(palette_.accent, theme::kWhite, std::clamp(highlight, 0.0f, 1.0f) * 0.35f);
}

void EdgeDockWindow::HandleInteractions() {
    // --- Barra de pestañas ------------------------------------------------------
    const float tabWidth = tabsRect_.Width() / static_cast<float>(kTabCount);
    for (int index = 0; index < kTabCount; ++index) {
        const Rectf tabRect{tabsRect_.left + tabWidth * index, tabsRect_.top,
                            tabsRect_.left + tabWidth * (index + 1), tabsRect_.bottom};
        ui_.HoverAmount(100 + index, tabRect, 18.0f);
        const bool clicked = ui_.Clicked(200 + index, tabRect);
        if (clicked) {
            ToggleTab(static_cast<PanelTab>(index));
            SetStatus(text::Format(L"Módulo %s", TabLabel(static_cast<PanelTab>(index))), false);
        }
    }

    // --- Barra de estado: indicadores de sistema y modo silencioso ----------------
    const Rectf focusButton{statusRect_.right - 92.0f, statusRect_.top + 2.0f, statusRect_.right,
                            statusRect_.bottom - 2.0f};
    ui_.HoverAmount(300, focusButton, 18.0f);
    if (ui_.Clicked(301, focusButton)) ToggleSystemFocus();

    if (tab_ == PanelTab::System) {
        const float buttonWidth = (contentRect_.Width() - 10.0f) / 2.0f;
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 2; ++column) {
                const int id = 400 + row * 2 + column;
                const Rectf button{contentRect_.left + column * (buttonWidth + 10.0f),
                                   contentRect_.top + 132.0f + row * 44.0f,
                                   contentRect_.left + column * (buttonWidth + 10.0f) + buttonWidth,
                                   contentRect_.top + 132.0f + row * 44.0f + 34.0f};
                ui_.HoverAmount(id, button, 18.0f);
                if (ui_.Clicked(id + 100, button)) RunClipCommand(kIdMenuBase + 21 + row * 2 + column, -1);
            }
        }
    }

    if (tab_ == PanelTab::Notes) {
        HandleNoteInteractions();
    }

    if (tab_ == PanelTab::Text) {
        const float buttonWidth = (contentRect_.Width() - 8.0f) / 3.0f;
        for (int index = 0; index < 12; ++index) {
            const int row = index / 3;
            const int column = index % 3;
            const Rectf button{contentRect_.left + column * (buttonWidth + 4.0f),
                               contentRect_.top + 92.0f + row * 34.0f,
                               contentRect_.left + column * (buttonWidth + 4.0f) + buttonWidth,
                               contentRect_.top + 92.0f + row * 34.0f + 28.0f};
            ui_.HoverAmount(600 + index, button, 20.0f);
            if (ui_.Clicked(700 + index, button)) RunTextOperation(static_cast<TextOperation>(index));
        }
    }

    // --- Lista de clips ---------------------------------------------------------
    if (tab_ != PanelTab::Clipboard || clips_ == nullptr) return;

    if (input_.wheelDelta != 0) {
        scrollTarget_ -= (static_cast<float>(input_.wheelDelta) / 120.0f) * 84.0f;
    }
    ClampScroll();
    const float factor = 1.0f - std::exp(-20.0f * std::max(0.001f, paintDelta_));
    scrollOffset_ += (scrollTarget_ - scrollOffset_) * factor;
    if (std::fabs(scrollTarget_ - scrollOffset_) < 0.5f) scrollOffset_ = scrollTarget_;

    hoveredClip_ = HitTestClipRow(input_.y);
    const float rowStride = theme::kRowHeight + theme::kRowGap;
    const int firstVisible = static_cast<int>(std::max(0.0f, std::floor(scrollOffset_ / rowStride)));
    const int lastVisible = std::min(static_cast<int>(clips_->VisibleCount()),
                                     firstVisible + static_cast<int>(contentRect_.Height() / rowStride) + 2);

    int scrollDeltaRows = 0;
    for (int index = firstVisible; index < lastVisible; ++index) {
        const float top = RowTop(static_cast<size_t>(index));
        const Rectf rowRect{contentRect_.left, top, contentRect_.right, top + theme::kRowHeight};
        const int rowId = 1000 + index;
        ui_.HoverAmount(rowId, rowRect, 20.0f);

        if (ui_.Clicked(rowId, rowRect)) {
            SelectClip(index);
            CopyVisibleClip(index);
        } else if (ui_.Clicked(rowId + 50000, rowRect, false)) {
            SelectClip(index);
        }
        if (ui_.DoubleClicked(rowId + 100000, rowRect)) {
            SelectClip(index);
            CopyVisibleClip(index);
            Collapse();
        }

        // Botón de anclaje en el extremo derecho de la fila.
        const Rectf pinRect{rowRect.right - 34.0f, rowRect.top + 6.0f, rowRect.right - 6.0f, rowRect.top + 34.0f};
        ui_.HoverAmount(rowId + 20000, pinRect, 22.0f);
        if (ui_.Clicked(rowId + 30000, pinRect)) {
            const bool pinned = clips_->TogglePin(static_cast<size_t>(index));
            SetStatus(pinned ? L"Clip anclado: no se podará ni sobrescribirá" : L"Clip liberado", !pinned);
            RefreshClips();
            return;
        }
    }

    // Rueda sobre la lista con fila resaltada: desplazamiento continuo suave.
    if (input_.wheelDelta > 0 && hoveredClip_ >= 0) scrollTarget_ += 0.0f;
    if (scrollDeltaRows != 0) {
        scrollTarget_ += static_cast<float>(scrollDeltaRows) * rowStride;
        ClampScroll();
    }

    // Arrastre hacia fuera del panel: pulsar una fila y moverse arranca un arrastre OLE.
    if (input_.down && dragCandidate_ < 0 && hoveredClip_ >= 0) {
        const float top = RowTop(static_cast<size_t>(hoveredClip_));
        const Rectf rowRect{contentRect_.left, top, contentRect_.right, top + theme::kRowHeight};
        if (ui_.Held(40000 + hoveredClip_, rowRect)) {
            dragCandidate_ = hoveredClip_;
            dragOriginX_ = input_.x;
            dragOriginY_ = input_.y;
        }
    }
    if (!input_.down) dragCandidate_ = -1;
    if (dragCandidate_ >= 0 && input_.down) {
        const float deltaX = input_.x - dragOriginX_;
        const float deltaY = input_.y - dragOriginY_;
        if ((deltaX * deltaX + deltaY * deltaY) > 36.0f && clips_ != nullptr) {
            const size_t index = static_cast<size_t>(dragCandidate_);
            dragCandidate_ = -1;
            const data::ClipEntry* entry = clips_->Visible(index);
            if (entry != nullptr) {
                std::vector<std::wstring> files;
                if (entry->kind == data::ClipKind::FilePath || entry->kind == data::ClipKind::ImagePath) {
                    files.push_back(entry->text);
                }
                // DoDragDrop toma el control del ratón hasta que se suelta: comportamiento
                // estándar del shell, el pintado se reanuda solo al volver.
                const DWORD effect = modules::StartTextDrag(hwnd_, entry->text, files);
                SetStatus(effect == DROPEFFECT_NONE ? L"Arrastre cancelado"
                                                    : L"Clip soltado en otra ventana",
                          effect == DROPEFFECT_NONE);
            }
        }
    }

    // Arrastre de la barra de desplazamiento.
    if (scrollMaximum_ > 1.0f) {
        const float trackTop = contentRect_.top;
        const float trackHeight = contentRect_.Height();
        const float thumbHeight = std::max(28.0f, trackHeight * (trackHeight / (trackHeight + scrollMaximum_)));
        const float thumbTop = trackTop + (trackHeight - thumbHeight) * (scrollOffset_ / scrollMaximum_);
        const Rectf thumb{contentRect_.right - theme::kScrollBarWidth - 1.0f, thumbTop,
                          contentRect_.right + 2.0f, thumbTop + thumbHeight};
        if (ui_.Held(900, thumb)) draggingScrollbar_ = true;
        if (draggingScrollbar_) {
            const float relative = (input_.y - trackTop) / std::max(1.0f, trackHeight - thumbHeight);
            scrollTarget_ = std::clamp(relative, 0.0f, 1.0f) * scrollMaximum_;
            scrollOffset_ = scrollTarget_;
            ClampScroll();
        }
        if (!input_.down) draggingScrollbar_ = false;
    }
}

void EdgeDockWindow::ShowClipContextMenu(int visibleIndex, POINT screenPoint) {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return;

    if (visibleIndex >= 0) {
        const data::ClipEntry* entry = clips_ != nullptr ? clips_->Visible(static_cast<size_t>(visibleIndex)) : nullptr;
        const bool pinned = entry != nullptr && entry->pinned;
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 1, pinned ? L"Liberar anclaje\tCtrl+P" : L"Anclar clip\tCtrl+P");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 2, L"Copiar al portapapeles\tCtrl+C");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 3, L"Eliminar del historial\tSupr");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 4, L"Limpiar texto de PDF/web");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 5, L"Convertir a MAY\u00DASCULAS");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 6, L"Convertir a snake_case");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 7, L"Convertir a camelCase");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 8, L"Extraer URL/email/c\u00F3digo");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 9, L"Envoltura a 100 columnas");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 10, L"Abrir ubicaci\u00F3n");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 11, L"Guardar como nota .md");
        if (entry != nullptr && entry->pinned) ::CheckMenuItem(menu, kIdMenuBase + 1, MF_BYCOMMAND | MF_CHECKED);
    } else {
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 21, L"Abrir carpeta de datos");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 22, L"Abrir config.json");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 23, L"Abrir carpeta de notas");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 24, L"Guardar historial como JSON");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 25, L"Alternar modo silencioso\tCtrl+Alt+F");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 26, L"Suspender procesos de la lista");
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 27, L"Reanudar procesos suspendidos");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kIdMenuBase + 28, L"Repliegar el panel");
    }

    // Menú oscuro: SetMenuInfo con pincel negro evita el gris del tema claro.
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    info.hbrBack = ::CreateSolidBrush(RGB(4, 6, 8));
    ::SetMenuInfo(menu, &info);

    ::SetForegroundWindow(hwnd_);
    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x,
                                          screenPoint.y, 0, hwnd_, nullptr);
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
    if (info.hbrBack != nullptr) ::DeleteObject(info.hbrBack);

    if (command != 0) RunClipCommand(static_cast<int>(command), visibleIndex);
}

void EdgeDockWindow::RunClipCommand(int commandId, int visibleIndex) {
    const int action = commandId - kIdMenuBase;

    // --- Acciones globales del panel (menú contextual y módulo SISTEMA) -----------
    if (action >= 21 && action <= 28) {
        switch (action) {
            case 21:
                SetStatus(sysutil::OpenFolder(paths::AppDataRoot()).ok ? L"Carpeta de datos abierta"
                                                              : L"No se pudo abrir la carpeta", true);
                break;
            case 22:
                SetStatus(sysutil::EditTextFile(paths::ConfigFile()).ok ? L"config.json abierto"
                                                               : L"No hay editor asociado a .json", true);
                break;
            case 23:
                SetStatus(sysutil::OpenFolder(config_->EffectiveNotesFolder()).ok ? L"Carpeta de notas abierta"
                                                                         : L"No se pudo abrir notas", true);
                break;
            case 24: {
                if (clips_ == nullptr) break;
                json::Value root = json::Value::MakeArray();
                for (const auto& entry : clips_->History()) {
                    json::Value item = json::Value::MakeObject();
                    item.Set("kind", json::Value(data::ClipKindLabel(entry.kind)));
                    item.Set("source", json::Value(text::ToUtf8(entry.source)));
                    item.Set("created_at", json::Value(entry.createdAt));
                    item.Set("hits", json::Value(entry.hits));
                    item.Set("pinned", json::Value(entry.pinned));
                    item.Set("text", json::Value(text::ToUtf8(entry.text)));
                    root.Push(std::move(item));
                }
                const std::wstring target = paths::Join(paths::AppDataRoot(), L"clips-export.json");
                const bool ok = paths::WriteTextFileAtomic(target, root.Dump(2) + "\n");
                SetStatus(ok ? L"Historial exportado a clips-export.json" : L"Fallo al exportar el historial", !ok);
                break;
            }
            case 25:
                ToggleFocusMode();
                break;
            case 26:
                SuspendConfiguredProcesses();
                break;
            case 27: {
                const size_t resumed = sysutil::ResumeProcesses(suspendedProcesses_);
                SetStatus(resumed > 0 ? text::Format(L"%llu procesos reanudados", static_cast<unsigned long long>(resumed))
                                      : L"No había procesos suspendidos por EdgeDock",
                          resumed == 0);
                break;
            }
            case 28:
                Collapse();
                break;
            default:
                break;
        }
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (clips_ == nullptr || visibleIndex < 0) return;
    const data::ClipEntry* entry = clips_->Visible(static_cast<size_t>(visibleIndex));
    if (entry == nullptr) return;
    const std::wstring original = entry->text;
    const size_t index = static_cast<size_t>(visibleIndex);

    switch (action) {
        case 1: {
            const bool pinned = clips_->TogglePin(index);
            SetStatus(pinned ? L"Clip anclado: protegido de la poda" : L"Clip liberado", !pinned);
            RefreshClips();
            break;
        }
        case 2:
            CopyVisibleClip(visibleIndex);
            break;
        case 3:
            if (clips_->DeleteAt(index)) {
                SetStatus(L"Clip eliminado", true);
                RefreshClips();
            }
            break;
        case 4: {
            modules::CleanOptions options;
            modules::CleanStats stats{};
            const std::wstring cleaned = modules::CleanText(original, options, &stats);
            if (clips_->ReplaceText(index, cleaned, true)) {
                SetStatus(text::Format(L"Limpiado: %d uniones, %d guiones, %d p\u00E1ginas \u00B7 -%llu ch",
                                       stats.linesJoined, stats.hyphenJoins, stats.pageNumbersRemoved,
                                       static_cast<unsigned long long>(stats.charsBefore > stats.charsAfter
                                                                           ? stats.charsBefore - stats.charsAfter : 0)),
                          false);
            }
            break;
        }
        case 5:
            if (clips_->ReplaceText(index, modules::ApplyCase(original, modules::CaseStyle::Upper), true))
                SetStatus(L"Convertido a MAY\u00DASCULAS y copiado", false);
            break;
        case 6:
            if (clips_->ReplaceText(index, modules::ToSnakeCase(original), true))
                SetStatus(L"Convertido a snake_case y copiado", false);
            break;
        case 7:
            if (clips_->ReplaceText(index, modules::ToCamelCase(original), true))
                SetStatus(L"Convertido a camelCase y copiado", false);
            break;
        case 8: {
            const modules::ExtractionResult found = modules::Extract(original);
            std::vector<std::wstring> joined;
            joined.insert(joined.end(), found.urls.begin(), found.urls.end());
            joined.insert(joined.end(), found.emails.begin(), found.emails.end());
            joined.insert(joined.end(), found.paths.begin(), found.paths.end());
            if (joined.empty()) {
                SetStatus(L"Sin URL, email ni rutas en el clip", true);
            } else {
                const std::wstring payload = text::Join(joined, L"\n");
                modules::ClipboardHook::CopyTextToClipboard(hwnd_, payload);
                SetStatus(text::Format(L"%llu coincidencias copiadas \u00B7 %s",
                                       static_cast<unsigned long long>(joined.size()),
                                       found.Summary().c_str()),
                          false);
            }
            break;
        }
        case 9:
            if (clips_->ReplaceText(index, modules::WrapForClipboard(original, 100), true))
                SetStatus(L"Reenvoltura a 100 columnas aplicada", false);
            break;
        case 10: {
            const data::ClipKind kind = entry->kind;
            if (kind == data::ClipKind::Url) {
                SetStatus(sysutil::OpenUrl(original).ok ? L"URL abierta en el navegador" : L"No se pudo abrir la URL", true);
            } else if (kind == data::ClipKind::FilePath || kind == data::ClipKind::ImagePath ||
                       kind == data::ClipKind::Files) {
                const std::wstring first = text::SplitLines(original).empty() ? original
                                                                            : text::SplitLines(original).front();
                const sysutil::LaunchResult result = sysutil::RevealInExplorer(first);
                SetStatus(result.ok ? L"Ubicaci\u00F3n abierta en el Explorador" : result.message, !result.ok);
            } else {
                SetStatus(L"Este clip no es una ruta ni una URL", true);
            }
            break;
        }
        case 11: {
            const std::wstring folder = config_->EffectiveNotesFolder();
            const std::vector<std::wstring> lines = text::SplitLines(text::Trim(original));
            const std::wstring title = lines.empty() ? std::wstring(L"clip")
                                                    : paths::SanitizeFileName(text::TruncateEllipsis(lines.front(), 48));
            const std::wstring path = paths::Join(
                folder, text::Format(L"%s-%s.md", title.c_str(), paths::TimestampForFile().c_str()));
            const std::string payload = text::ToUtf8(text::Format(L"# %s\n\n_%s \u00B7 %s_\n\n%s\n",
                                                                 title.c_str(),
                                                                 data::ClipKindLabel(entry->kind),
                                                                 paths::RelativeTime(entry->createdAt).c_str(),
                                                                 original.c_str()));
            const bool ok = paths::WriteTextFileAtomic(path, payload);
            SetStatus(ok ? text::Format(L"Nota creada: %s", paths::FileNameOf(path).c_str())
                         : L"No se pudo escribir la nota",
                      !ok);
            break;
        }
        default:
            break;
    }
    UpdateTextPreview();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::RunTextOperation(TextOperation operation) {
    std::wstring source;
    if (clips_ != nullptr && selectedClip_ >= 0) {
        const data::ClipEntry* entry = clips_->Visible(static_cast<size_t>(selectedClip_));
        if (entry != nullptr) source = entry->text;
    }
    if (source.empty()) {
        modules::ClipboardHook::ReadClipboardText(hwnd_, source);
    }
    if (source.empty()) {
        SetStatus(L"No hay texto: selecciona un clip o copia algo primero", true);
        return;
    }

    modules::CleanOptions options;
    modules::CleanStats stats{};
    std::wstring result;
    switch (operation) {
        case TextOperation::CleanPdf: result = modules::CleanText(source, options, &stats); break;
        case TextOperation::CleanMarkdown: result = modules::ToCleanMarkdown(source, options); break;
        case TextOperation::SingleParagraph: result = modules::ToSingleParagraph(source, options); break;
        case TextOperation::CaseCamel: result = modules::ToCamelCase(source); break;
        case TextOperation::CaseSnake: result = modules::ToSnakeCase(source); break;
        case TextOperation::CaseKebab: result = modules::ToKebabCase(source); break;
        case TextOperation::CaseUpper: result = text::ToUpper(source); break;
        case TextOperation::CaseLower: result = text::ToLower(source); break;
        case TextOperation::CaseTitle: result = modules::ToTitleCase(source); break;
        case TextOperation::StripFormatting: result = modules::StripFormatting(source); break;
        case TextOperation::Wrap: result = modules::WrapForClipboard(source, 100); break;
        case TextOperation::Extract: {
            extraction_ = modules::Extract(source);
            std::vector<std::wstring> joined;
            joined.insert(joined.end(), extraction_.urls.begin(), extraction_.urls.end());
            joined.insert(joined.end(), extraction_.emails.begin(), extraction_.emails.end());
            joined.insert(joined.end(), extraction_.codeBlocks.begin(), extraction_.codeBlocks.end());
            joined.insert(joined.end(), extraction_.paths.begin(), extraction_.paths.end());
            result = joined.empty() ? std::wstring(L"[sin coincidencias]") : text::Join(joined, L"\n\n");
            break;
        }
    }

    lastTextOperation_ = static_cast<int>(operation);
    processedPreview_ = result;
    textStats_ = sysutil::AnalyzeText(result);
    modules::ClipboardHook::CopyTextToClipboard(hwnd_, result);
    SetStatus(text::Format(L"Resultado listo \u00B7 %s \u00B7 copiado", textStats_.Compact().c_str()), false);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::UpdateTextPreview() {
    std::wstring source;
    if (clips_ != nullptr && selectedClip_ >= 0) {
        const data::ClipEntry* entry = clips_->Visible(static_cast<size_t>(selectedClip_));
        if (entry != nullptr) source = entry->text;
    }
    if (source.empty()) modules::ClipboardHook::ReadClipboardText(hwnd_, source);
    processedPreview_ = source;
    lastTextOperation_ = -1;
    textStats_ = sysutil::AnalyzeText(source);
    extraction_ = modules::Extract(source);
}

// -----------------------------------------------------------------------------
// Módulo SISTEMA
// -----------------------------------------------------------------------------

void EdgeDockWindow::UpdateSystemSnapshot(bool force) {
    const ULONGLONG now = ::GetTickCount64();
    if (!force && now - lastSnapshotTick_ < 900) return;
    lastSnapshotTick_ = now;
    snapshot_ = sysutil::SampleSystem();
}

void EdgeDockWindow::SuspendConfiguredProcesses() {
    if (config_->cpuHogs.empty()) {
        SetStatus(L"La lista cpu_hogs de config.json está vacía", true);
        return;
    }
    const std::vector<sysutil::SuspendedProcess> suspended =
        sysutil::SuspendProcessesByNames(config_->cpuHogs);
    for (const auto& process : suspended) suspendedProcesses_.push_back(process);
    SetStatus(suspended.empty()
                  ? L"Ningún proceso de la lista está en ejecución"
                  : text::Format(L"%llu procesos suspendidos (NtSuspendProcess)",
                                 static_cast<unsigned long long>(suspended.size())),
              suspended.empty());
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::ReleaseSuspendedProcesses() {
    const size_t resumed = sysutil::ResumeProcesses(suspendedProcesses_);
    if (resumed > 0) {
        SetStatus(text::Format(L"%llu procesos reanudados", static_cast<unsigned long long>(resumed)), false);
    }
}

void EdgeDockWindow::ToggleFocusMode() {
    if (focusModeActive_) {
        const bool restored = sysutil::RestoreFocusAssist(focusState_);
        focusModeActive_ = false;
        SetStatus(restored ? L"Modo silencioso desactivado: ajustes restaurados"
                           : L"Restauración parcial del modo silencioso",
                  !restored);
        if (!suspendedProcesses_.empty()) ReleaseSuspendedProcesses();
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const sysutil::FocusPriority priority = config_->focusAssistPriority == 2
                                                ? sysutil::FocusPriority::AlarmsOnly
                                                : (config_->focusAssistPriority == 0
                                                       ? sysutil::FocusPriority::Off
                                                       : sysutil::FocusPriority::PriorityOnly);
    const bool ok = sysutil::SetFocusAssist(priority, focusState_);
    focusModeActive_ = ok && focusState_.active;
    SetStatus(ok ? text::Format(L"Modo silencioso \u00B7 %s%s",
                                sysutil::FocusPriorityLabel(priority).c_str(),
                                focusState_.registryPatched ? L" (registro verificado)"
                                                            : L" (avisos silenciados)")
                 : L"No se pudo activar el modo silencioso",
              !ok);
    if (focusModeActive_ && config_->suspendCpuHogsOnFocus) SuspendConfiguredProcesses();
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::ToggleSystemFocus() {
    ToggleFocusMode();
}

// -----------------------------------------------------------------------------
// Dibujo
// -----------------------------------------------------------------------------

void EdgeDockWindow::Paint() {
    const ULONGLONG now = ::GetTickCount64();
    if (lastPaintTick_ != 0) {
        paintDelta_ = std::clamp(static_cast<float>(now - lastPaintTick_) / 1000.0f, 0.001f, 0.12f);
    }
    lastPaintTick_ = now;

    // Arena del frame: todo el texto formateado del pase sale de aquí y se recicla después.
    mem::FrameArena::BeginFrame();

    UpdateInputState();
    ui_.Begin(paintDelta_, input_);

    windowHeightDip_ = static_cast<int>(std::lround(PxToDip(windowHeight_)));
    ComputeLayout();
    HandleInteractions();
    LayoutSearchControl();

    if (!renderer_.BeginFrame()) {
        ui_.End();
        ResetTransientInput();
        mem::FrameArena::EndFrame();
        return;
    }

    renderer_.SetPalette(palette_);
    renderer_.Clear(theme::kBlack);

    if (expandProgress_ < 0.03f) {
        PaintCollapsedStripe();
        renderer_.EndFrame();
        ui_.End();
        ResetTransientInput();
        mem::FrameArena::EndFrame();
        return;
    }

    PaintHeader();
    PaintTabs();
    switch (tab_) {
        case PanelTab::Clipboard: PaintClipboardTab(); break;
        case PanelTab::Text: PaintTextTab(); break;
        case PanelTab::Notes: PaintNotesTab(); break;
        case PanelTab::System: PaintSystemTab(); break;
    }
    PaintStatusBar();

    renderer_.EndFrame();
    ui_.End();
    ResetTransientInput();
    mem::FrameArena::EndFrame();
}

void EdgeDockWindow::PaintCollapsedStripe() {
    const float width = std::max(2.0f, config_->collapsedDip);
    const float right = contentOriginX_ + config_->panelWidthDip;
    const Rectf stripe{right - width, 0.0f, right, static_cast<float>(windowHeightDip_)};
    renderer_.FillRect(stripe, theme::WithAlpha(palette_.accent, 0.75f));
    renderer_.FillRect(Rectf{stripe.left, 0.0f, stripe.left + 1.0f, stripe.bottom},
                       theme::WithAlpha(palette_.secondary, 0.55f));

    // Asa central: pista visual de que el panel está ahí.
    const float centerY = stripe.bottom * 0.5f;
    const Rectf handle{stripe.left - 3.0f, centerY - 26.0f, stripe.right, centerY + 26.0f};
    renderer_.FillRect(handle, theme::WithAlpha(palette_.accent, 0.30f), 3.0f);
}

void EdgeDockWindow::PaintHeader() {
    TextStyle title;
    title.size = 17.0f;
    title.weight = 700;
    title.letterSpacing = true;
    TextStyle subtitle;
    subtitle.size = 10.5f;
    subtitle.weight = 400;
    TextStyle stat;
    stat.size = 10.0f;
    stat.weight = 600;
    stat.font = FontRole::Mono;
    stat.align = TextAlign::Right;

    const Rectf titleRect = headerRect_;
    renderer_.DrawText(L"EDGEDOCK", titleRect, title, palette_.accent);
    renderer_.DrawText(L"STUDIO", Rectf{titleRect.left + 118.0f, titleRect.top, titleRect.right, titleRect.top + 22.0f},
                       title, theme::kText);

    const std::wstring subtitleText = captureSummary_.empty()
                                          ? L"Omni-panel de utilidades \u00B7 borde derecho"
                                          : (L"Último clip \u00B7 " + captureSummary_);
    renderer_.DrawText(subtitleText, Rectf{titleRect.left, titleRect.top + 21.0f, titleRect.right - 96.0f,
                                           titleRect.top + 38.0f},
                       subtitle, theme::kMuted);

    const std::wstring liveStat = text::Format(L"CPU %s  RAM %s", sysutil::FormatPercent(snapshot_.cpuPercent).c_str(),
                                               sysutil::FormatBytes(snapshot_.usedMemoryBytes).c_str());
    renderer_.DrawText(liveStat, Rectf{titleRect.right - 200.0f, titleRect.top + 3.0f, titleRect.right,
                                       titleRect.top + 20.0f},
                       stat, theme::kTextSoft);

    // Separador inferior con degradado cian -> púrpura.
    const float y = headerRect_.bottom + 3.0f;
    renderer_.FillRect(Rectf{headerRect_.left, y, headerRect_.left + headerRect_.Width() * 0.55f, y + 1.0f},
                       theme::WithAlpha(palette_.accent, 0.55f));
    renderer_.FillRect(Rectf{headerRect_.left + headerRect_.Width() * 0.55f, y, headerRect_.right, y + 1.0f},
                       theme::WithAlpha(palette_.secondary, 0.45f));
}

void EdgeDockWindow::PaintTabs() {
    const float tabWidth = tabsRect_.Width() / static_cast<float>(kTabCount);
    const size_t visible = clips_ != nullptr ? clips_->VisibleCount() : 0;
    const size_t pinned = clips_ != nullptr ? clips_->PinnedCount() : 0;

    TextStyle label;
    label.size = 10.5f;
    label.weight = 600;
    label.letterSpacing = true;
    label.align = TextAlign::Center;
    label.valign = VAlign::Middle;

    for (int index = 0; index < kTabCount; ++index) {
        const PanelTab tab = static_cast<PanelTab>(index);
        const Rectf tabRect{tabsRect_.left + tabWidth * index, tabsRect_.top,
                            tabsRect_.left + tabWidth * (index + 1), tabsRect_.bottom};
        const bool active = tab_ == tab;
        const float highlight = ui_.HoverAmount(100 + index, tabRect, 18.0f);

        if (!active && highlight > 0.01f) {
            renderer_.FillRect(tabRect.Inset(2.0f, 6.0f), theme::WithAlpha(palette_.accent, 0.07f * highlight), 5.0f);
        }
        std::wstring textLabel = TabLabel(tab);
        if (tab == PanelTab::Clipboard && visible > 0) {
            textLabel += text::Format(L"  %llu", static_cast<unsigned long long>(visible));
        } else if (tab == PanelTab::System && focusModeActive_) {
            textLabel += L"  \u25CF";
        }
        const theme::Rgba color = active ? palette_.accent
                                         : theme::Mix(theme::kMuted, theme::kText, highlight);
        renderer_.DrawText(textLabel, tabRect, label, color);

        const float underlineY = tabsRect_.bottom - 1.0f;
        if (active) {
            renderer_.FillRect(Rectf{tabRect.left + 6.0f, underlineY, tabRect.right - 6.0f, underlineY + 2.0f},
                               palette_.accent, 1.0f);
        }
    }

    // Contador de anclados bajo la barra.
    if (pinned > 0) {
        TextStyle badge;
        badge.size = 9.5f;
        badge.font = FontRole::Mono;
        badge.align = TextAlign::Right;
        renderer_.DrawText(text::Format(L"\u2691 %llu anclados", static_cast<unsigned long long>(pinned)),
                           Rectf{tabsRect_.left, tabsRect_.bottom + 2.0f, tabsRect_.right, searchRect_.top - 4.0f},
                           badge, theme::WithAlpha(palette_.secondary, 0.85f));
    }
}

void EdgeDockWindow::PaintClipboardTab() {
    const bool searchHasText =
        searchEdit_ != nullptr && ::GetWindowTextLengthW(searchEdit_) > 0;
    const float searchHighlight = searchHasText ? 1.0f : ui_.HoverAmount(350, searchRect_, 16.0f);
    ui_.DrawNeonFrame(renderer_, searchRect_, palette_, searchHighlight, 8.0f, true);

    const float lensX = searchRect_.left + 15.0f;
    const float lensY = searchRect_.CenterY() - 1.0f;
    const theme::Rgba lensColor = theme::WithAlpha(palette_.accent, 0.85f);
    renderer_.StrokeRect(Rectf{lensX - 5.0f, lensY - 5.0f, lensX + 5.0f, lensY + 5.0f}, lensColor, 5.0f, 1.4f);
    renderer_.DrawLine(lensX + 4.0f, lensY + 4.0f, lensX + 8.0f, lensY + 8.0f, lensColor, 1.4f);

    if (clips_ == nullptr) return;

    if (clips_->VisibleCount() == 0) {
        TextStyle empty;
        empty.size = 12.0f;
        empty.align = TextAlign::Center;
        empty.wrap = true;
        const std::wstring message = clips_->History().empty()
                                        ? L"Historial vac\u00EDo.\nCopia algo y aparecer\u00E1 aqu\u00ED autom\u00E1ticamente."
                                        : L"Sin resultados para esta b\u00FAsqueda.";
        renderer_.DrawText(message,
                           Rectf{contentRect_.left, contentRect_.CenterY() - 34.0f, contentRect_.right,
                                 contentRect_.CenterY() + 10.0f},
                           empty, theme::kMuted);
        return;
    }

    renderer_.PushClip(contentRect_);
    const float rowStride = theme::kRowHeight + theme::kRowGap;
    const int totalRows = static_cast<int>(clips_->VisibleCount());
    const int firstVisible = static_cast<int>(std::max(0.0f, std::floor(scrollOffset_ / rowStride)));
    const int visibleRows = static_cast<int>(contentRect_.Height() / rowStride) + 2;
    const int lastVisible = std::min(totalRows, firstVisible + visibleRows);

    TextStyle preview;
    preview.size = 11.5f;
    preview.wrap = false;
    TextStyle meta;
    meta.size = 9.5f;
    meta.font = FontRole::Mono;

    for (int index = firstVisible; index < lastVisible; ++index) {
        const data::ClipEntry* entry = clips_->Visible(static_cast<size_t>(index));
        if (entry == nullptr) continue;

        const float top = RowTop(static_cast<size_t>(index));
        const Rectf row{contentRect_.left, top, contentRect_.right, top + theme::kRowHeight};
        const float highlight = ui_.HoverAmount(1000 + index, row, 20.0f);
        const bool selected = index == selectedClip_;
        const bool pinned = entry->pinned;

        const theme::Rgba background = selected ? theme::kRowSelected
                                               : theme::Mix(theme::kPanel, theme::kRowHover, highlight);
        renderer_.FillRect(row, background, 8.0f);

        if (selected || highlight > 0.02f) {
            const theme::Rgba bar = selected ? palette_.accent
                                            : theme::WithAlpha(palette_.accent, 0.55f * highlight);
            renderer_.FillRect(Rectf{row.left, row.top + 8.0f, row.left + 2.0f, row.bottom - 8.0f}, bar, 1.0f);
        }

        const theme::Rgba kindColor = entry->kind == data::ClipKind::Code
                                          ? palette_.secondary
                                          : ((entry->kind == data::ClipKind::Url ||
                                              entry->kind == data::ClipKind::Email ||
                                              entry->kind == data::ClipKind::ImagePath)
                                                 ? palette_.accent
                                                 : theme::kTextSoft);
        ui_.DrawKindBadge(renderer_,
                          Rectf{row.left + 12.0f, row.top + 11.0f, row.left + 60.0f, row.top + 25.0f},
                          data::ClipKindLabel(entry->kind), kindColor);

        if (entry->kind == data::ClipKind::Code) preview.font = FontRole::Mono;
        else preview.font = FontRole::Ui;

        std::wstring firstLine;
        std::wstring secondLine;
        const size_t newline = entry->text.find(L'\n');
        if (newline == std::wstring::npos) {
            firstLine = text::TruncateEllipsis(entry->text, 160);
        } else {
            firstLine = text::TruncateEllipsis(entry->text.substr(0, newline), 160);
            std::wstring rest = entry->text.substr(newline + 1);
            const size_t secondNewline = rest.find(L'\n');
            if (secondNewline != std::wstring::npos) rest.resize(secondNewline);
            secondLine = text::TruncateEllipsis(text::Trim(rest), 190);
        }
        renderer_.DrawText(firstLine, Rectf{row.left + 68.0f, row.top + 10.0f, row.right - 44.0f, row.top + 28.0f},
                           preview, selected ? theme::kWhite : theme::kText);
        if (!secondLine.empty()) {
            renderer_.DrawText(secondLine,
                               Rectf{row.left + 68.0f, row.top + 28.0f, row.right - 44.0f, row.top + 44.0f},
                               preview, theme::kMuted);
        }

        // Cadena formateada en la arena del frame: cero heap en el bucle de filas.
        const mem::FrameString metaText = mem::FrameStringV(
            L"%s \u00B7 %s \u00B7 x%lld", paths::RelativeTime(entry->createdAt).c_str(),
            entry->source.c_str(), static_cast<long long>(entry->hits));
        const std::wstring_view metaView(metaText.data(), metaText.size());
        renderer_.DrawText(metaView,
                           Rectf{row.left + 68.0f, row.bottom - 22.0f, row.right - 44.0f, row.bottom - 6.0f},
                           meta, theme::kMuted);

        const Rectf pinRect{row.right - 36.0f, row.top + 8.0f, row.right - 8.0f, row.top + 36.0f};
        const float pinHighlight = ui_.HoverAmount(1000 + index + 20000, pinRect, 22.0f);
        renderer_.FillRect(pinRect, pinned ? theme::WithAlpha(palette_.accent, 0.18f)
                                          : theme::WithAlpha(theme::kWhite, 0.03f + 0.07f * pinHighlight),
                           4.0f);
        ui_.DrawPin(renderer_, pinRect.CenterX(), pinRect.CenterY(), 13.0f,
                    pinned ? palette_.accent : theme::kMuted, pinned);

        renderer_.FillRect(Rectf{row.left + 12.0f, row.bottom + 3.0f, row.right - 12.0f, row.bottom + 4.0f},
                           theme::kHairline);
    }
    renderer_.PopClip();

    if (scrollMaximum_ > 1.0f) {
        const float trackHeight = contentRect_.Height();
        const float thumbHeight = std::max(28.0f, trackHeight * (trackHeight / (trackHeight + scrollMaximum_)));
        const float thumbTop = contentRect_.top +
                               (trackHeight - thumbHeight) * (scrollOffset_ / scrollMaximum_);
        renderer_.FillRect(Rectf{contentRect_.right - 4.0f, contentRect_.top, contentRect_.right - 2.0f,
                                 contentRect_.bottom},
                           theme::WithAlpha(theme::kWhite, 0.05f));
        renderer_.FillRect(Rectf{contentRect_.right - 5.0f, thumbTop, contentRect_.right - 1.0f,
                                 thumbTop + thumbHeight},
                           theme::WithAlpha(palette_.accent, draggingScrollbar_ ? 0.95f : 0.6f), 1.5f);
    }
}

void EdgeDockWindow::PaintTextTab() {
    TextStyle section;
    section.size = 9.5f;
    section.weight = 700;
    section.letterSpacing = true;
    TextStyle value;
    value.size = 11.5f;
    value.font = FontRole::Mono;
    TextStyle body;
    body.size = 11.5f;
    body.wrap = true;
    TextStyle button;
    button.size = 10.0f;
    button.weight = 600;
    button.align = TextAlign::Center;
    button.valign = VAlign::Middle;

    // --- Bloque de métricas ----------------------------------------------------
    const Rectf metrics{contentRect_.left, contentRect_.top, contentRect_.right, contentRect_.top + 80.0f};
    ui_.DrawNeonFrame(renderer_, metrics, palette_, 0.0f, 8.0f, true);
    renderer_.DrawText(L"M\u00C9TRICAS EN VIVO",
                       Rectf{metrics.left + 12.0f, metrics.top + 8.0f, metrics.right - 12.0f, metrics.top + 22.0f},
                       section, palette_.accent);
    renderer_.DrawText(text::Format(L"%llu palabras \u00B7 %llu caracteres (%llu sin espacios)",
                                    static_cast<unsigned long long>(textStats_.words),
                                    static_cast<unsigned long long>(textStats_.chars),
                                    static_cast<unsigned long long>(textStats_.charsNoSpaces)),
                       Rectf{metrics.left + 12.0f, metrics.top + 26.0f, metrics.right - 12.0f, metrics.top + 44.0f},
                       value, theme::kText);
    renderer_.DrawText(text::Format(L"%llu l\u00EDneas \u00B7 %llu p\u00E1rrafos \u00B7 %llu frases",
                                    static_cast<unsigned long long>(textStats_.lines),
                                    static_cast<unsigned long long>(textStats_.paragraphs),
                                    static_cast<unsigned long long>(textStats_.sentences)),
                       Rectf{metrics.left + 12.0f, metrics.top + 44.0f, metrics.right - 12.0f, metrics.top + 60.0f},
                       value, theme::kTextSoft);
    renderer_.DrawText(text::Format(L"Lectura %s \u00B7 habla %s \u00B7 250/150 ppm",
                                    sysutil::FormatDurationShort(textStats_.readingSeconds).c_str(),
                                    sysutil::FormatDurationShort(textStats_.speakingSeconds).c_str()),
                       Rectf{metrics.left + 12.0f, metrics.top + 60.0f, metrics.right - 12.0f, metrics.top + 74.0f},
                       value, theme::WithAlpha(palette_.secondary, 0.9f));

    // --- Rejilla de operaciones ------------------------------------------------
    static const wchar_t* kOperationLabels[] = {
        L"LIMPIAR PDF", L"MD LIMPIO", L"1 P\u00C1RRAFO",
        L"camelCase", L"snake_case", L"kebab-case",
        L"MAY\u00DASCULAS", L"min\u00FAsculas", L"T\u00EDtulo",
        L"EXTRAER", L"SIN FORMATO", L"100 COL"};

    const float buttonWidth = (contentRect_.Width() - 8.0f) / 3.0f;
    for (int index = 0; index < 12; ++index) {
        const int row = index / 3;
        const int column = index % 3;
        const Rectf rect{contentRect_.left + column * (buttonWidth + 4.0f),
                         contentRect_.top + 92.0f + row * 34.0f,
                         contentRect_.left + column * (buttonWidth + 4.0f) + buttonWidth,
                         contentRect_.top + 92.0f + row * 34.0f + 28.0f};
        const float highlight = ui_.HoverAmount(600 + index, rect, 20.0f);
        ui_.DrawNeonFrame(renderer_, rect, palette_, highlight, 5.0f, true);
        renderer_.DrawText(kOperationLabels[index], rect, button,
                           theme::Mix(theme::kTextSoft, palette_.accent, highlight));
    }

    // --- Extracción regex ------------------------------------------------------
    renderer_.DrawText(L"EXTRACCI\u00D3N REGEX",
                       Rectf{contentRect_.left, contentRect_.top + 240.0f, contentRect_.right,
                             contentRect_.top + 254.0f},
                       section, palette_.secondary);
    renderer_.DrawText(extraction_.Summary(),
                       Rectf{contentRect_.left, contentRect_.top + 254.0f, contentRect_.right,
                             contentRect_.top + 270.0f},
                       value, theme::kTextSoft);

    // --- Vista previa del resultado -------------------------------------------
    const Rectf previewPanel{contentRect_.left, contentRect_.top + 278.0f, contentRect_.right,
                             contentRect_.bottom};
    ui_.DrawNeonFrame(renderer_, previewPanel, palette_, 0.0f, 8.0f, true);
    renderer_.DrawText(lastTextOperation_ >= 0 ? L"RESULTADO" : L"TEXTO DE ORIGEN",
                       Rectf{previewPanel.left + 12.0f, previewPanel.top + 8.0f, previewPanel.right - 12.0f,
                             previewPanel.top + 22.0f},
                       section, palette_.accent);
    renderer_.PushClip(previewPanel.Inset(12.0f, 12.0f));
    renderer_.DrawText(processedPreview_.empty() ? L"[sin texto: selecciona un clip]" : processedPreview_,
                       Rectf{previewPanel.left + 12.0f, previewPanel.top + 26.0f, previewPanel.right - 12.0f,
                             previewPanel.bottom - 10.0f},
                       body, processedPreview_.empty() ? theme::kMuted : theme::kText);
    renderer_.PopClip();
}

void EdgeDockWindow::PaintSystemTab() {
    TextStyle section;
    section.size = 9.5f;
    section.weight = 700;
    section.letterSpacing = true;
    TextStyle value;
    value.size = 11.0f;
    value.font = FontRole::Mono;
    TextStyle button;
    button.size = 10.0f;
    button.weight = 600;
    button.align = TextAlign::Center;
    button.valign = VAlign::Middle;

    const Rectf resources{contentRect_.left, contentRect_.top, contentRect_.right, contentRect_.top + 118.0f};
    ui_.DrawNeonFrame(renderer_, resources, palette_, 0.0f, 8.0f, true);
    renderer_.DrawText(L"RECURSOS",
                       Rectf{resources.left + 12.0f, resources.top + 8.0f, resources.right - 12.0f,
                             resources.top + 22.0f},
                       section, palette_.accent);

    const auto drawBar = [&](const Rectf& track, double ratio, theme::Rgba color) {
        renderer_.FillRect(track, theme::WithAlpha(theme::kWhite, 0.06f), 2.0f);
        const float filled = static_cast<float>(std::clamp(ratio, 0.0, 1.0)) * track.Width();
        if (filled > 1.0f) renderer_.FillRect(Rectf{track.left, track.top, track.left + filled, track.bottom}, color, 2.0f);
    };

    const double memoryRatio = snapshot_.totalMemoryBytes > 0
                                   ? static_cast<double>(snapshot_.usedMemoryBytes) /
                                         static_cast<double>(snapshot_.totalMemoryBytes)
                                   : 0.0;
    renderer_.DrawText(text::Format(L"CPU %s", sysutil::FormatPercent(snapshot_.cpuPercent).c_str()),
                       Rectf{resources.left + 12.0f, resources.top + 26.0f, resources.right - 12.0f,
                             resources.top + 40.0f},
                       value, theme::kTextSoft);
    drawBar(Rectf{resources.left + 96.0f, resources.top + 30.0f, resources.right - 12.0f,
                  resources.top + 38.0f},
            snapshot_.cpuPercent / 100.0, palette_.accent);

    renderer_.DrawText(text::Format(L"RAM %s / %s", sysutil::FormatBytes(snapshot_.usedMemoryBytes).c_str(),
                                    sysutil::FormatBytes(snapshot_.totalMemoryBytes).c_str()),
                       Rectf{resources.left + 12.0f, resources.top + 46.0f, resources.right - 12.0f,
                             resources.top + 60.0f},
                       value, theme::kTextSoft);
    drawBar(Rectf{resources.left + 12.0f, resources.top + 62.0f, resources.right - 12.0f,
                  resources.top + 70.0f},
            memoryRatio, palette_.secondary);

    renderer_.DrawText(text::Format(L"%llu procesos \u00B7 %llu hilos \u00B7 encendido %s",
                                    static_cast<unsigned long long>(snapshot_.processCount),
                                    static_cast<unsigned long long>(snapshot_.threadCount),
                                    sysutil::FormatDurationShort(
                                        static_cast<double>(snapshot_.uptimeSeconds)).c_str()),
                       Rectf{resources.left + 12.0f, resources.top + 78.0f, resources.right - 12.0f,
                             resources.top + 94.0f},
                       value, theme::kMuted);
    renderer_.DrawText(text::Format(L"Python: %s",
                                    pythonInterpreter_.empty() ? L"no encontrado en PATH"
                                                               : pythonInterpreter_.c_str()),
                       Rectf{resources.left + 12.0f, resources.top + 96.0f, resources.right - 12.0f,
                             resources.top + 112.0f},
                       value, theme::kMuted);

    static const wchar_t* kSystemLabels[] = {
        L"CARPETA DE DATOS", L"CONFIG.JSON",
        L"CARPETA DE NOTAS", L"EXPORTAR HISTORIAL",
        L"MODO SILENCIOSO", L"SUSPENDER CPU HOGS",
        L"REANUDAR PROCESOS", L"REPLEGAR PANEL"};

    const float buttonWidth = (contentRect_.Width() - 10.0f) / 2.0f;
    for (int index = 0; index < 8; ++index) {
        const int row = index / 2;
        const int column = index % 2;
        const Rectf rect{contentRect_.left + column * (buttonWidth + 10.0f),
                         contentRect_.top + 132.0f + row * 44.0f,
                         contentRect_.left + column * (buttonWidth + 10.0f) + buttonWidth,
                         contentRect_.top + 132.0f + row * 44.0f + 34.0f};
        const float highlight = ui_.HoverAmount(400 + index, rect, 18.0f);
        const bool activeButton = index == 4 && focusModeActive_;
        ui_.DrawNeonFrame(renderer_, rect, palette_, activeButton ? 1.0f : highlight, 5.0f, true);
        std::wstring textLabel = kSystemLabels[index];
        if (index == 4) textLabel = focusModeActive_ ? L"DESACTIVAR SILENCIO" : L"ACTIVAR SILENCIO";
        renderer_.DrawText(textLabel, rect, button,
                           activeButton ? palette_.accent
                                        : theme::Mix(theme::kTextSoft, palette_.accent, highlight));
    }

    const Rectf footer{contentRect_.left, contentRect_.top + 316.0f, contentRect_.right,
                       contentRect_.bottom};
    ui_.DrawNeonFrame(renderer_, footer, palette_, 0.0f, 8.0f, true);
    renderer_.DrawText(L"ESTADO",
                       Rectf{footer.left + 12.0f, footer.top + 8.0f, footer.right - 12.0f, footer.top + 22.0f},
                       section, palette_.secondary);
    renderer_.DrawText(text::Format(L"Modo silencioso: %s (%s)",
                                    focusModeActive_ ? L"ACTIVO" : L"inactivo",
                                    focusModeActive_
                                        ? (focusState_.registryPatched ? L"registro verificado" : L"avisos silenciados")
                                        : L"notificaciones normales"),
                       Rectf{footer.left + 12.0f, footer.top + 24.0f, footer.right - 12.0f, footer.top + 40.0f},
                       value, focusModeActive_ ? palette_.accent : theme::kTextSoft);
    renderer_.DrawText(text::Format(L"Procesos suspendidos por EdgeDock: %llu",
                                    static_cast<unsigned long long>(suspendedProcesses_.size())),
                       Rectf{footer.left + 12.0f, footer.top + 40.0f, footer.right - 12.0f, footer.top + 56.0f},
                       value, theme::kTextSoft);

    std::wstring suspendedNames;
    for (size_t i = 0; i < suspendedProcesses_.size() && i < 6; ++i) {
        if (!suspendedNames.empty()) suspendedNames += L", ";
        suspendedNames += suspendedProcesses_[i].name;
    }
    renderer_.DrawText(suspendedNames.empty() ? L"Ninguno: la lista cpu_hogs se suspende cuando lo pidas"
                                              : suspendedNames,
                       Rectf{footer.left + 12.0f, footer.top + 56.0f, footer.right - 12.0f, footer.top + 72.0f},
                       value, theme::kMuted);
}

void EdgeDockWindow::PaintStatusBar() {
    renderer_.FillRect(Rectf{statusRect_.left, statusRect_.top - 2.0f, statusRect_.right, statusRect_.top - 1.0f},
                       theme::kHairline);

    TextStyle status;
    status.size = 10.0f;
    status.font = FontRole::Mono;
    TextStyle button;
    button.size = 9.5f;
    button.weight = 700;
    button.letterSpacing = true;
    button.align = TextAlign::Center;
    button.valign = VAlign::Middle;

    const std::wstring defaultStatus = clips_ != nullptr
                                           ? text::Format(L"%llu clips \u00B7 base %s \u00B7 %llu capturas",
                                                          static_cast<unsigned long long>(clips_->History().size()),
                                                          UiState::FormatBytes(
                                                              static_cast<uint64_t>(std::max<int64_t>(
                                                                  0, database_ != nullptr ? database_->DatabaseSizeBytes() : 0)))
                                                              .c_str(),
                                                          static_cast<unsigned long long>(clips_->CapturedCount()))
                                           : std::wstring(L"EdgeDock Studio");

    renderer_.DrawText(statusText_.empty() ? defaultStatus : statusText_,
                       Rectf{statusRect_.left, statusRect_.top, statusRect_.right - 100.0f, statusRect_.bottom},
                       status, statusWarn_ ? theme::kWarn : theme::kMuted);

    const Rectf focusButton{statusRect_.right - 92.0f, statusRect_.top + 2.0f, statusRect_.right,
                            statusRect_.bottom - 2.0f};
    const float highlight = ui_.HoverAmount(300, focusButton, 18.0f);
    ui_.DrawNeonFrame(renderer_, focusButton, palette_, focusModeActive_ ? 1.0f : highlight, 5.0f, true);
    renderer_.DrawText(L"FOCUS 2PX", focusButton, button,
                       focusModeActive_ ? palette_.accent : theme::Mix(theme::kMuted, theme::kText, highlight));
}

// -----------------------------------------------------------------------------
// Módulo NOTAS (Scratchpad)
// -----------------------------------------------------------------------------
namespace {

struct NoteRowContext {
    const modules::ScratchpadManager* manager = nullptr;
    theme::Palette palette{};
};

// Pintor de filas sin std::function: sólo recibe el contexto ya preparado.
void PaintNoteRow(void* context, Renderer& renderer, std::size_t index, const Rectf& rect, float hover,
                  bool selected) {
    auto* rowContext = static_cast<NoteRowContext*>(context);
    if (rowContext == nullptr || rowContext->manager == nullptr) return;
    const modules::ScratchNote* note = rowContext->manager->NoteAt(index);
    if (note == nullptr) return;

    renderer.FillRect(rect, selected ? theme::kRowSelected : theme::Mix(theme::kPanel, theme::kRowHover, hover),
                      6.0f);
    if (selected || hover > 0.02f) {
        renderer.FillRect(Rectf{rect.left, rect.top + 6.0f, rect.left + 2.0f, rect.bottom - 6.0f},
                          selected ? rowContext->palette.accent
                                   : theme::WithAlpha(rowContext->palette.accent, 0.5f * hover),
                          1.0f);
    }

    TextStyle title;
    title.size = 11.5f;
    title.weight = 600;
    TextStyle preview;
    preview.size = 10.0f;
    TextStyle meta;
    meta.size = 9.0f;
    meta.font = FontRole::Mono;
    meta.align = TextAlign::Right;

    renderer.DrawText(note->title, Rectf{rect.left + 12.0f, rect.top + 7.0f, rect.right - 96.0f, rect.top + 24.0f},
                      title, selected ? theme::kWhite : theme::kText);
    renderer.DrawText(note->preview.empty() ? L"(nota vac\u00EDa)" : note->preview,
                      Rectf{rect.left + 12.0f, rect.top + 24.0f, rect.right - 96.0f, rect.top + 40.0f}, preview,
                      theme::kMuted);
    const mem::FrameString noteMeta = mem::FrameStringV(L"%s \u00B7 %s",
                                                       paths::RelativeTime(note->modifiedAt).c_str(),
                                                       UiState::FormatBytes(note->bytes).c_str());
    renderer.DrawText(std::wstring_view(noteMeta.data(), noteMeta.size()),
                      Rectf{rect.left, rect.top + 8.0f, rect.right - 10.0f, rect.top + 22.0f}, meta,
                      theme::kMuted);
}

} // namespace

void EdgeDockWindow::RefreshNotes() {
    notes_.Refresh();
    notesList_.target = 0.0f;
    SetStatus(text::Format(L"%llu notas en %s", static_cast<unsigned long long>(notes_.NoteCount()),
                           notes_.Folder().c_str()),
              false);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::LayoutNotesEditor() {
    if (notes_.EditorHandle() == nullptr) return;
    const bool visible = expandProgress_ >= 0.75f && tab_ == PanelTab::Notes;
    if (!visible) {
        notes_.ShowEditor(false);
        return;
    }

    const float scale = DipToPx(1.0f);
    RECT client{};
    client.left = static_cast<LONG>(std::lround((contentRect_.left + 6.0f) * scale));
    client.top = static_cast<LONG>(std::lround((notesListBottomDip_ + 10.0f) * scale));
    client.right = static_cast<LONG>(std::lround((contentRect_.right - 6.0f) * scale));
    client.bottom = static_cast<LONG>(std::lround((contentRect_.bottom - 4.0f) * scale));
    notes_.LayoutEditor(client, dpi_);
    notes_.ShowEditor(true);
}

void EdgeDockWindow::HandleNoteInteractions() {
    if (!targetExpanded_) return;

    const float buttonHeight = 26.0f;
    const float buttonWidth = (contentRect_.Width() - 18.0f) / 4.0f;
    static const wchar_t* kNoteButtons[4] = {L"NUEVA", L"GUARDAR", L"BORRAR", L"CARPETA"};
    for (int index = 0; index < 4; ++index) {
        const Rectf button{contentRect_.left + index * (buttonWidth + 6.0f), contentRect_.top + 22.0f,
                           contentRect_.left + index * (buttonWidth + 6.0f) + buttonWidth,
                           contentRect_.top + 22.0f + buttonHeight};
        if (ui_.Clicked(800 + index, button)) {
            HandleNoteCommand(index);
            return;
        }
    }

    // El listado se pinta dentro del pase de Direct2D (PaintNotesTab): aquí solo se
    // resuelven los botones, que no necesitan el renderizador.
}

void EdgeDockWindow::HandleNoteCommand(int actionId) {
    switch (actionId) {
        case 0:
            if (notes_.NewNote()) SetStatus(notes_.LastStatus(), false);
            break;
        case 1: {
            const bool saved = notes_.FlushNow();
            SetStatus(saved ? L"Nota guardada en disco" : L"No se pudo guardar la nota", !saved);
            break;
        }
        case 2: {
            const int active = notes_.ActiveIndex();
            if (active < 0) {
                SetStatus(L"No hay nota activa que borrar", true);
                break;
            }
            SetStatus(notes_.DeleteNote(static_cast<size_t>(active)) ? L"Nota borrada del disco"
                                                                     : notes_.LastStatus(),
                      false);
            break;
        }
        case 3:
            SetStatus(sysutil::OpenFolder(notes_.Folder()).ok ? L"Carpeta de notas abierta"
                                                              : L"No se pudo abrir la carpeta",
                      true);
            break;
        default:
            break;
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void EdgeDockWindow::PaintNotesTab() {
    TextStyle section;
    section.size = 9.5f;
    section.weight = 700;
    section.letterSpacing = true;
    TextStyle info;
    info.size = 10.0f;
    info.font = FontRole::Mono;

    renderer_.DrawText(L"NOTAS R\u00C1PIDAS",
                       Rectf{contentRect_.left, contentRect_.top - 2.0f, contentRect_.right,
                             contentRect_.top + 14.0f},
                       section, palette_.accent);

    const size_t pending = notes_.PendingWrites();
    const std::wstring saveState =
        notes_.Dirty() ? text::Format(L"editando \u00B7 %llu escrituras \u00B7 %llu en curso",
                               static_cast<unsigned long long>(notes_.SavedCount()),
                               static_cast<unsigned long long>(pending))
                : text::Format(L"%llu notas \u00B7 %llu escrituras \u00B7 %s",
                               static_cast<unsigned long long>(notes_.NoteCount()),
                               static_cast<unsigned long long>(notes_.SavedCount()),
                               notes_.LastSaveUnix() > 0
                                   ? paths::FormatLocalTime(notes_.LastSaveUnix(), L"%H:%M:%S").c_str()
                                   : L"pendiente");
    renderer_.DrawText(saveState,
                       Rectf{contentRect_.left, contentRect_.top + 12.0f, contentRect_.right,
                             contentRect_.top + 24.0f},
                       info, notes_.Dirty() ? theme::kWarn : theme::kMuted);

    static const wchar_t* kNoteButtons[4] = {L"NUEVA", L"GUARDAR", L"BORRAR", L"CARPETA"};
    const float buttonHeight = 26.0f;
    const float buttonWidth = (contentRect_.Width() - 18.0f) / 4.0f;
    ButtonStyle buttonStyle;
    buttonStyle.kind = ButtonKind::Ghost;
    buttonStyle.text.size = 9.5f;
    buttonStyle.text.weight = 600;
    buttonStyle.text.align = TextAlign::Center;
    buttonStyle.text.valign = VAlign::Middle;

    for (int index = 0; index < 4; ++index) {
        const Rectf button{contentRect_.left + index * (buttonWidth + 6.0f), contentRect_.top + 22.0f,
                           contentRect_.left + index * (buttonWidth + 6.0f) + buttonWidth,
                           contentRect_.top + 22.0f + buttonHeight};
        const float highlight = ui_.HoverAmount(800 + index, button, 22.0f);
        ui_.DrawNeonFrame(renderer_, button, palette_, highlight, 4.0f, true);
        renderer_.DrawText(kNoteButtons[index], button, buttonStyle.text,
                           theme::Mix(theme::kMuted, palette_.accent, highlight));
    }

    // --- Lista de notas (widget de lista virtualizada, dentro del pase de dibujo) ---
    const Rectf listRect{contentRect_.left, contentRect_.top + 58.0f, contentRect_.right,
                         notesListBottomDip_};
    NoteRowContext rowContext;
    rowContext.manager = &notes_;
    rowContext.palette = palette_;

    ListRowStyle rowStyle;
    rowStyle.rowHeight = 48.0f;
    rowStyle.gap = 4.0f;
    rowStyle.radius = 6.0f;

    if (notes_.NoteCount() == 0) {
        TextStyle empty;
        empty.size = 11.0f;
        empty.align = TextAlign::Center;
        empty.wrap = true;
        renderer_.DrawText(L"Sin notas todav\u00EDa.\nSuelta texto aqu\u00ED o pulsa NUEVA.",
                           Rectf{listRect.left, listRect.top + 30.0f, listRect.right, listRect.top + 76.0f},
                           empty, theme::kMuted);
    } else {
        const ListViewResult result =
            ui_.ScrollList(renderer_, 900, listRect, rowStyle, notes_.NoteCount(), notesList_, &PaintNoteRow,
                           &rowContext, notes_.ActiveIndex(), palette_);
        if (result.clicked) {
            // Abrir la nota en el mismo pase es seguro: el EDIT es una ventana hija y el
            // texto de la nota se aplica con SetWindowTextW, sin tocar el lienzo D2D.
            if (notes_.Open(result.clickedIndex)) {
                SetStatus(text::Format(L"Nota abierta \u00B7 %s", notes_.LastStatus().c_str()), false);
            }
        }
    }

    if (notes_.DropActive()) {
        const Rectf dropZone{listRect.left, listRect.top, listRect.right, listRect.bottom};
        renderer_.StrokeRect(dropZone, theme::WithAlpha(palette_.accent, 0.9f), 6.0f, 1.6f);
        renderer_.DrawText(L"SUELTA PARA CREAR UNA NOTA .MD",
                           Rectf{dropZone.left + 10.0f, dropZone.top + 6.0f, dropZone.right - 10.0f,
                                 dropZone.top + 22.0f},
                           section, palette_.accent);
    }

    renderer_.DrawText(text::TruncateEllipsis(notes_.Folder(), 58),
                       Rectf{contentRect_.left, contentRect_.bottom - 14.0f, contentRect_.right,
                             contentRect_.bottom},
                       info, theme::kMuted);
}
} // namespace edgedock::ui
