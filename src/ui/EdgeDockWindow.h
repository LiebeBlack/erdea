#pragma once
// EdgeDock Studio :: ui/EdgeDockWindow.h
// Panel lateral anclado al borde derecho: se contrae a una franja de 2 px, se despliega al
// chocar el cursor contra el borde, anima con SetTimer + SetWindowPos y se dibuja por
// Direct2D sobre fondo negro puro.

#include <windows.h>

#include <string>
#include <vector>

#include "core/AsyncWorker.h"
#include "core/Config.h"
#include "data/ClipDatabase.h"
#include "modules/ClipboardHook.h"
#include "modules/DragDrop.h"
#include "modules/ScratchpadManager.h"
#include "modules/SystemUtils.h"
#include "modules/TextProcessor.h"
#include "ui/Renderer.h"
#include "ui/Widgets.h"

namespace edgedock::ui {

enum class PanelTab { Clipboard = 0, Text = 1, Notes = 2, System = 3 };

enum class TextOperation {
    CleanPdf = 0,
    CleanMarkdown,
    SingleParagraph,
    CaseCamel,
    CaseSnake,
    CaseKebab,
    CaseUpper,
    CaseLower,
    CaseTitle,
    Extract,
    StripFormatting,
    Wrap
};

class EdgeDockWindow {
public:
    EdgeDockWindow() = default;
    ~EdgeDockWindow();
    EdgeDockWindow(const EdgeDockWindow&) = delete;
    EdgeDockWindow& operator=(const EdgeDockWindow&) = delete;

    bool Create(HINSTANCE instance, data::ClipDatabase& database, modules::ClipboardHook& clips,
                Config& config, AsyncWorker& worker);
    void Destroy();

    HWND Handle() const { return hwnd_; }
    bool IsExpanded() const { return targetExpanded_; }

    void Expand();
    void Collapse();
    void TogglePanel();
    void FocusSearch();
    void RefreshClips();
    void NotifyNewClip();
    void SetStatus(const std::wstring& text, bool warn = false);
    void ToggleFocusMode();
    void ReleaseSuspendedProcesses();
    void ToggleTab(PanelTab tab);
    void RefreshNotes();
    bool DropActive() const { return notes_.DropActive(); }

    static constexpr const wchar_t* kClassName = L"EdgeDockStudio.PanelWindow";
    // La App avisa por aquí (p. ej. tras podar la base) para que la lista se recargue.
    static constexpr UINT kMsgRefreshClips = WM_APP + 0x42;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    // --- Mensajes ---------------------------------------------------------------
    void OnCreate();
    void OnDestroy();
    void OnPaint();
    void OnTimer(UINT_PTR timerId);
    void OnSize(int width, int height);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnMouseWheel(int delta);
    void OnLeftButtonDown(int x, int y);
    void OnLeftButtonUp(int x, int y);
    void OnLeftButtonDoubleClick(int x, int y);
    void OnRightButtonUp(int x, int y);
    void OnKeyDown(WPARAM key);
    void OnCommand(WPARAM controlId, LPARAM controlHandle);
    void OnDisplayChange();

    // --- Geometría y animación ---------------------------------------------------
    void UpdateMonitorBounds();
    void PollEdgeTrigger();
    void StartAnimation();
    void TickAnimation();
    void ApplyWindowRect(float visibleDip);
    void ComputeLayout();
    float DipToPx(float value) const;
    float PxToDip(int value) const;

    // --- Dibujo -----------------------------------------------------------------
    void Paint();
    void PaintHeader();
    void PaintTabs();
    void PaintClipboardTab();
    void PaintTextTab();
    void PaintNotesTab();
    void PaintSystemTab();
    void PaintStatusBar();
    void PaintCollapsedStripe();
    theme::Rgba Accent(float highlight) const;

    // --- Interacción -------------------------------------------------------------
    void UpdateInputState();
    void HandleInteractions();
    void ResetTransientInput();
    int HitTestClipRow(float dipY) const;
    void ClampScroll();
    float RowTop(size_t visibleIndex) const;
    void EnsureVisible(int visibleIndex);
    void SelectClip(int visibleIndex, bool focusSearch = false);
    void CopyVisibleClip(int visibleIndex);
    void ShowClipContextMenu(int visibleIndex, POINT screenPoint);
    void RunClipCommand(int commandId, int visibleIndex);
    void CreateSearchControl();
    void LayoutSearchControl();
    void CommitSearch();
    void ClearSearch();
    void UpdateTextPreview();
    void RunTextOperation(TextOperation operation);
    void HandleNoteInteractions();
    void LayoutNotesEditor();
    void HandleNoteCommand(int actionId);
    void UpdateSystemSnapshot(bool force);
    void ToggleSystemFocus();
    void SuspendConfiguredProcesses();

    // --- Estado -----------------------------------------------------------------
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND searchEdit_ = nullptr;
    HWND tooltip_ = nullptr;
    Renderer renderer_{};
    UiState ui_{};
    Config* config_ = nullptr;
    data::ClipDatabase* database_ = nullptr;
    modules::ClipboardHook* clips_ = nullptr;
    AsyncWorker* worker_ = nullptr;
    theme::Palette palette_{};

    PanelTab tab_ = PanelTab::Clipboard;
    float expandProgress_ = 0.0f;
    bool targetExpanded_ = false;
    bool animationRunning_ = false;
    ULONGLONG animationStartTick_ = 0;
    float animationStartProgress_ = 0.0f;
    ULONGLONG collapseDeadline_ = 0;
    bool trackingLeave_ = false;
    bool pointerInside_ = false;
    int dpi_ = 96;
    RECT monitorRect_{};
    RECT workArea_{};
    int anchorX_ = 0;
    int anchorY_ = 0;
    int windowHeight_ = 0;
    int windowWidthPx_ = 0;

    // Rectángulos de trabajo (en DIP, ya desplazados por la animación de entrada).
    Rectf headerRect_{};
    Rectf tabsRect_{};
    Rectf searchRect_{};
    Rectf contentRect_{};
    Rectf statusRect_{};
    float contentOriginX_ = 0.0f;
    float contentWidthDip_ = 0.0f;
    float notesListBottomDip_ = 0.0f;
    int windowHeightDip_ = 0;
    HFONT searchFont_ = nullptr;
    float paintDelta_ = 0.016f;
    ULONGLONG lastPaintTick_ = 0;
    bool refreshPending_ = false;
    bool busyWithSearch_ = false;

    InputState input_{};
    POINT cursorPx_{};
    int hoveredTab_ = -1;
    int hoveredClip_ = -1;
    int selectedClip_ = -1;
    float scrollOffset_ = 0.0f;
    float scrollTarget_ = 0.0f;
    float scrollMaximum_ = 0.0f;
    bool draggingScrollbar_ = false;
    float hoveredButton_ = -1.0f;
    // Arrastre hacia fuera: se arma al pulsar una fila y se dispara al superar el umbral.
    int dragCandidate_ = -1;
    float dragOriginX_ = 0.0f;
    float dragOriginY_ = 0.0f;

    std::wstring statusText_;
    bool statusWarn_ = false;
    ULONGLONG statusUntil_ = 0;
    std::wstring captureSummary_;

    int lastTextOperation_ = -1;
    std::wstring processedPreview_;
    modules::ExtractionResult extraction_{};
    sysutil::TextStats textStats_{};

    modules::ScratchpadManager notes_{};
    modules::PanelDropTarget* dropTarget_ = nullptr;
    ListViewState notesList_{};

    sysutil::SystemSnapshot snapshot_{};
    ULONGLONG lastSnapshotTick_ = 0;
    sysutil::FocusState focusState_{};
    bool focusModeActive_ = false;
    std::vector<sysutil::SuspendedProcess> suspendedProcesses_;
    std::wstring pythonInterpreter_;

    static constexpr UINT_PTR kTimerAnimation = 1;
    static constexpr UINT_PTR kTimerEdge = 2;
    static constexpr UINT_PTR kTimerStatus = 3;
    static constexpr UINT_PTR kTimerStats = 4;
    static constexpr UINT kEdgePollMs = 40;
    static constexpr UINT kAnimationFrameMs = 16;
    static constexpr int kWmEdgeDockTick = WM_APP + 0x40;
};

} // namespace edgedock::ui
